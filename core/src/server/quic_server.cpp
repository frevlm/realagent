/*
 * quic_server.cpp — UDP socket → quiche → h3 事件 → 路由表
 */
#include "server/quic_server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <quiche.h>

namespace realagent {

namespace {

/* 一条在途请求。状态属于流，不属于连接：一条连接上可以并发多个请求。 */
struct Request {
    std::string method;
    std::string path;
    std::string body;
};

struct Conn {
    quiche_conn *conn = nullptr;
    quiche_h3_conn *h3 = nullptr;
    sockaddr_storage peer{};
    socklen_t peer_len = 0;
    std::map<uint64_t, Request> requests; // 按流 id
    int64_t events_stream = -1;           // GET /events 的那条流；-1 = 没订阅
};

bool ensure_cert(const std::string &cert_file, const std::string &key_file)
{
    if (access(cert_file.c_str(), R_OK) == 0 && access(key_file.c_str(), R_OK) == 0) return true;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 365 -nodes "
             "-subj '/CN=realagent' 2>/dev/null",
             key_file.c_str(), cert_file.c_str());
    return system(cmd) == 0;
}

quiche_h3_header header(const char *name, const char *value)
{
    return {(uint8_t *)name, strlen(name), (uint8_t *)value, strlen(value)};
}

} // namespace

struct QuicServer::Impl {
    QuicServerConfig cfg;
    std::atomic<int> &clients;
    int fd = -1;
    sockaddr_in addr{};
    quiche_config *config = nullptr;
    quiche_h3_config *h3_config = nullptr;
    std::map<uint64_t, Conn> conns; // 按服务端 SCID 前 8 字节
    std::map<std::string, Handler> routes;
    std::function<void()> tick;
    bool running = false;

    Impl(QuicServerConfig c, std::atomic<int> &n) : cfg(std::move(c)), clients(n) {}
    ~Impl();

    bool setup();
    void recv_packet(const uint8_t *buf, size_t n, const sockaddr_storage &peer, socklen_t peer_len);
    void poll_h3(Conn &c);
    void finish(Conn &c, uint64_t sid, const Request &req);
    void respond(Conn &c, uint64_t sid, const std::string &body, const char *status = "200");
    void flush(Conn &c);
    void service_all();
};

QuicServer::Impl::~Impl()
{
    for (auto &[_, c] : conns)
    {
        if (c.h3) quiche_h3_conn_free(c.h3);
        if (c.conn) quiche_conn_free(c.conn);
    }
    if (h3_config) quiche_h3_config_free(h3_config);
    if (config) quiche_config_free(config);
    if (fd >= 0) close(fd);
}

bool QuicServer::Impl::setup()
{
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return false;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // 一台机器一个 core：端口就是那把锁
    if (bind(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        if (errno == EADDRINUSE)
            fprintf(stderr, "[server] core 已在 %d 运行\n", cfg.port);
        else
            perror("bind");
        return false;
    }

    config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    h3_config = quiche_h3_config_new();
    if (!config || !h3_config)
    {
        fprintf(stderr, "[server] quiche 配置创建失败\n");
        return false;
    }
    quiche_config_set_application_protos(config, (const uint8_t *)"\x02h3", 3);
    quiche_config_grease(config, false);
    // 10 秒：客户端断了要尽快知道
    quiche_config_set_max_idle_timeout(config, 10000);
    quiche_config_set_initial_max_data(config, 1048576);
    quiche_config_set_initial_max_stream_data_bidi_local(config, 262144);
    quiche_config_set_initial_max_stream_data_bidi_remote(config, 262144);
    quiche_config_set_initial_max_streams_bidi(config, 100);
    quiche_config_set_initial_max_streams_uni(config, 100); // QPACK / 控制流

    if (!ensure_cert(cfg.cert_file, cfg.key_file) ||
        quiche_config_load_cert_chain_from_pem_file(config, cfg.cert_file.c_str()) < 0 ||
        quiche_config_load_priv_key_from_pem_file(config, cfg.key_file.c_str()) < 0)
    {
        fprintf(stderr, "[server] 证书生成或加载失败\n");
        return false;
    }
    fprintf(stderr, "[server] QUIC/HTTP3（quiche）运行在 127.0.0.1:%d\n", cfg.port);
    return true;
}

void QuicServer::Impl::flush(Conn &c)
{
    uint8_t out[65536];
    quiche_send_info si{};
    for (ssize_t n; (n = quiche_conn_send(c.conn, out, sizeof(out), &si)) > 0;)
        sendto(fd, out, n, 0, (sockaddr *)&c.peer, c.peer_len);
}

void QuicServer::Impl::respond(Conn &c, uint64_t sid, const std::string &body, const char *status)
{
    quiche_h3_header h[] = {header(":status", status), header("content-type", "application/json")};
    quiche_h3_send_response(c.h3, c.conn, sid, h, 2, false);
    quiche_h3_send_body(c.h3, c.conn, sid, (const uint8_t *)body.data(), body.size(), true);
}

void QuicServer::Impl::finish(Conn &c, uint64_t sid, const Request &req)
{
    // 推送流：只回头，不结束流。路径里可能带查询串（?client_id=...），只比前缀
    if (req.method == "GET" && req.path.starts_with("/events"))
    {
        if (c.events_stream < 0) clients.fetch_add(1);
        c.events_stream = (int64_t)sid;
        quiche_h3_header h[] = {header(":status", "200"), header("content-type", "text/event-stream")};
        quiche_h3_send_response(c.h3, c.conn, sid, h, 2, false);
        return;
    }
    const auto it = routes.find(req.method + " " + req.path);
    if (it == routes.end()) return respond(c, sid, R"({"error":"not found"})", "404");
    // 请求体形状不对（字段类型错了）会让 json 抛异常；一个坏请求不许带走常驻服务
    try
    {
        respond(c, sid, it->second(req.body));
    } catch (const std::exception &e)
    {
        respond(c, sid, R"({"ok":false,"error":"bad request"})", "400");
        fprintf(stderr, "[server] %s %s: %s\n", req.method.c_str(), req.path.c_str(), e.what());
    }
}

void QuicServer::Impl::poll_h3(Conn &c)
{
    quiche_h3_event *ev;
    for (int64_t s; (s = quiche_h3_conn_poll(c.h3, c.conn, &ev)) >= 0; quiche_h3_event_free(ev))
    {
        const uint64_t sid = (uint64_t)s;
        switch (quiche_h3_event_type(ev))
        {
            case QUICHE_H3_EVENT_HEADERS:
                c.requests[sid] = Request{};
                quiche_h3_event_for_each_header(
                    ev,
                    [](uint8_t *name, size_t nlen, uint8_t *value, size_t vlen, void *arg) {
                        auto *r = static_cast<Request *>(arg);
                        const std::string_view n((const char *)name, nlen);
                        if (n == ":method") r->method.assign((const char *)value, vlen);
                        if (n == ":path") r->path.assign((const char *)value, vlen);
                        return 0;
                    },
                    &c.requests[sid]);
                break;
            case QUICHE_H3_EVENT_DATA: {
                uint8_t buf[65536];
                for (ssize_t n; (n = quiche_h3_recv_body(c.h3, c.conn, sid, buf, sizeof(buf))) > 0;)
                    c.requests[sid].body.append((const char *)buf, n);
                break;
            }
            case QUICHE_H3_EVENT_FINISHED: {
                const Request req = std::move(c.requests[sid]);
                c.requests.erase(sid);
                finish(c, sid, req);
                break;
            }
            default:
                break;
        }
    }
}

void QuicServer::Impl::recv_packet(const uint8_t *buf, size_t n, const sockaddr_storage &peer,
                                   socklen_t peer_len)
{
    uint8_t type;
    uint32_t version;
    uint8_t scid[QUICHE_MAX_CONN_ID_LEN], dcid[QUICHE_MAX_CONN_ID_LEN], token[256];
    size_t scid_len = sizeof(scid), dcid_len = sizeof(dcid), token_len = sizeof(token);
    if (quiche_header_info(buf, n, QUICHE_MAX_CONN_ID_LEN, &version, &type, scid, &scid_len, dcid,
                           &dcid_len, token, &token_len) < 0)
        return;

    uint64_t key = 0;
    memcpy(&key, dcid, dcid_len < 8 ? dcid_len : 8);
    auto it = conns.find(key);
    if (it == conns.end())
    {
        Conn c;
        c.peer = peer;
        c.peer_len = peer_len;
        c.conn = quiche_accept(dcid, dcid_len, nullptr, 0, (sockaddr *)&addr, sizeof(addr),
                               (sockaddr *)&peer, peer_len, config);
        if (!c.conn) return;
        it = conns.emplace(key, std::move(c)).first;
    }
    Conn &c = it->second;

    quiche_recv_info ri = {(sockaddr *)&peer, peer_len, (sockaddr *)&addr, sizeof(addr)};
    if (quiche_conn_recv(c.conn, (uint8_t *)buf, n, &ri) < 0) return;
    if (!c.h3 && quiche_conn_is_established(c.conn))
        c.h3 = quiche_h3_conn_new_with_transport(c.conn, h3_config);
    if (c.h3) poll_h3(c);
    flush(c);
}

/* 每个连接：走定时器（不叫它 idle timeout 永远不到期）、回收已关的、发挂起的。 */
void QuicServer::Impl::service_all()
{
    for (auto it = conns.begin(); it != conns.end();)
    {
        Conn &c = it->second;
        quiche_conn_on_timeout(c.conn);
        if (!quiche_conn_is_closed(c.conn))
        {
            flush(c);
            ++it;
            continue;
        }
        if (c.events_stream >= 0) clients.fetch_sub(1);
        if (c.h3) quiche_h3_conn_free(c.h3);
        quiche_conn_free(c.conn);
        it = conns.erase(it);
    }
}

QuicServer::QuicServer(QuicServerConfig cfg) : impl_(std::make_unique<Impl>(std::move(cfg), clients_)) {}
QuicServer::~QuicServer() = default;

void QuicServer::route(const std::string &method, const std::string &path, Handler h)
{
    impl_->routes[method + " " + path] = std::move(h);
}

void QuicServer::on_tick(std::function<void()> f) { impl_->tick = std::move(f); }

void QuicServer::push_event(const std::string &type, const std::string &payload)
{
    if (!impl_->running) return;
    const std::string frame = "event: " + type + "\ndata: " + payload + "\n\n";
    for (auto &[_, c] : impl_->conns)
    {
        if (c.events_stream < 0 || !c.h3 || !quiche_conn_is_established(c.conn)) continue;
        quiche_h3_send_body(c.h3, c.conn, (uint64_t)c.events_stream, (const uint8_t *)frame.data(),
                            frame.size(), false);
        impl_->flush(c); // 立即发：事件要流式到达
    }
}

void QuicServer::run()
{
    Impl &s = *impl_;
    if (!s.setup()) return;
    s.running = true;

    pollfd pfd{s.fd, POLLIN, 0};
    uint8_t buf[65536];
    while (s.running)
    {
        if (s.tick) s.tick();
        const int rv = poll(&pfd, 1, 1000);
        if (rv < 0 && errno != EINTR) break;
        if (rv > 0 && (pfd.revents & POLLIN))
        {
            sockaddr_storage peer{};
            socklen_t peer_len = sizeof(peer);
            const ssize_t n = recvfrom(s.fd, buf, sizeof(buf), 0, (sockaddr *)&peer, &peer_len);
            if (n > 0) s.recv_packet(buf, (size_t)n, peer, peer_len);
        }
        s.service_all();
    }
    s.running = false;
}

} // namespace realagent
