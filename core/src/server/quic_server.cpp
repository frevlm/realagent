/*
 * quic_server.cpp — QUIC/HTTP3 服务端实现（quiche）
 *
 * 基于 quiche C API 的 HTTP/3 服务端。事件驱动：
 *  UDP socket → quiche 引擎 → h3 事件（HEADERS/BODY/FINISHED）→ 响应
 *
 * 依赖：quiche（brew install cloudflare-quiche）
 */
#include "server/quic_server.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <quiche.h>

#include "json.hpp"

namespace realagent {

/* 一条在途请求。HTTP/3 一条连接上多个流并发，请求状态因此属于**流**，不属于连接：
 * 挂在连接上就只有一份槽，两个并发请求会互相覆盖方法/路径/正文，响应还会发错流。 */
struct QuicRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct QuicConn {
    quiche_conn *conn = nullptr;
    quiche_h3_conn *h3 = nullptr;
    sockaddr_storage peer{};
    socklen_t peer_len = 0;
    uint64_t scid = 0;
    /* 在途请求，按流 id 索引：HEADERS 建、DATA 追加、FINISHED 用完即删 */
    std::map<uint64_t, QuicRequest> requests;
    /* 事件推送流（GET /events 订阅） */
    bool events_subscribed = false;
    int64_t events_stream = -1;
    /* 订阅推送流时报上来的客户端身份（ADR-0021）。这条连接死了就是这个客户端没了 */
    std::string client_id;
};

struct QuicServer::Impl {
    int fd = -1;
    quiche_config *config = nullptr;
    quiche_h3_config *h3_config = nullptr;
    std::map<uint64_t, QuicConn> conns;
    QuicServerConfig cfg;
    QuicCallbacks cbs;
    bool running = false;

    ~Impl() { cleanup(); }
    void cleanup();
};

QuicServer::QuicServer(const QuicServerConfig &cfg) : cfg_(cfg) {}
QuicServer::~QuicServer() = default;

/* 推送事件到所有订阅 /events 的客户端（SSE 语义，事件循环线程内调用） */
void QuicServer::push_event(const std::string &type, const std::string &payload)
{
    if (!impl_ || !impl_->running) return;
    const std::string frame = "event: " + type + "\ndata: " + payload + "\n\n";
    for (auto &[_, c] : impl_->conns)
    {
        if (!c.h3 || !c.events_subscribed || c.events_stream < 0) continue;
        if (!c.conn || !quiche_conn_is_established(c.conn)) continue;
        quiche_h3_send_body(c.h3, c.conn, (uint64_t)c.events_stream,
                            (const uint8_t *)frame.data(), frame.size(), false);
        // 立即发送（agent 事件要流式到达，不等事件循环下次 tick）
        uint8_t out[65536];
        quiche_send_info si{};
        ssize_t sent = quiche_conn_send(c.conn, out, sizeof(out), &si);
        if (sent > 0) sendto(impl_->fd, out, sent, 0, (sockaddr *)&c.peer, c.peer_len);
    }
}

/* ==================== 证书 ==================== */

static bool ensure_cert(const std::string &cert_file, const std::string &key_file)
{
    if (access(cert_file.c_str(), R_OK) == 0 && access(key_file.c_str(), R_OK) == 0) return true;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s -days 365 -nodes "
             "-subj '/CN=realagent' 2>/dev/null",
             key_file.c_str(), cert_file.c_str());
    return system(cmd) == 0;
}

/* ==================== 响应辅助 ==================== */

/* 发 JSON 响应（application/json；事件循环线程内调用）。
 * 流 id 由调用方从当前 h3 事件传入——不存连接上，就不会发错流。 */
static void send_json(QuicConn &c, uint64_t stream_id, const std::string &body,
                      const char *status = "200")
{
    quiche_h3_header resp_headers[] = {
        {(uint8_t *)":status", 7, (uint8_t *)status, strlen(status)},
        {(uint8_t *)"content-type", 12, (uint8_t *)"application/json", 16},
        {(uint8_t *)"server", 6, (uint8_t *)"realagent", 9},
    };
    quiche_h3_send_response(c.h3, c.conn, stream_id, resp_headers, 3, false);
    quiche_h3_send_body(c.h3, c.conn, stream_id,
                        (const uint8_t *)body.data(), body.size(), true);
}

/* ==================== 事件循环 ==================== */

void QuicServer::Impl::cleanup()
{
    for (auto &[_, c] : conns)
    {
        if (c.h3) quiche_h3_conn_free(c.h3);
        if (c.conn) quiche_conn_free(c.conn);
    }
    conns.clear();
    if (h3_config) quiche_h3_config_free(h3_config);
    if (config) quiche_config_free(config);
    if (fd >= 0) close(fd);
    fd = -1;
}

void QuicServer::run()
{
    impl_ = std::make_unique<Impl>();
    if (!impl_) return;
    impl_->cfg = cfg_;
    impl_->cbs = cbs_;
    running_ = true;
    impl_->running = true;

    // UDP socket
    impl_->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (impl_->fd < 0)
    {
        perror("socket");
        running_ = false;
        return;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(impl_->fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        // core 一台机器一个实例（ADR-0019）。端口本来就是那把锁，bind 是内核帮忙做的
        // 原子检查——不另立 pid 文件：那是在内核已经解决的问题上再造一把会变陈旧的锁，
        // 进程被 kill -9 之后它还在，那才是真麻烦。这里改的只有报错文本
        if (errno == EADDRINUSE)
            fprintf(stderr, "[server] core 已在 %d 运行\n", cfg_.port);
        else
            perror("bind");
        running_ = false;
        return;
    }

    // quiche 配置
    impl_->config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!impl_->config)
    {
        fprintf(stderr, "[server] quiche_config_new 失败\n");
        running_ = false;
        return;
    }
    impl_->h3_config = quiche_h3_config_new();
    if (!impl_->h3_config)
    {
        fprintf(stderr, "[server] h3_config 创建失败\n");
        running_ = false;
        return;
    }
    quiche_config_set_application_protos(impl_->config, (const uint8_t *)"\x02h3", 3);
    quiche_config_grease(impl_->config, false); // 兼容性：禁用 GREASE 流
    // 10 秒：连接死了要让 core 尽快知道，好开始数那 60 秒（ADR-0021）。
    // QUIC 自己会保活，闲着不发请求的客户端不会因此掉线
    quiche_config_set_max_idle_timeout(impl_->config, 10000);
    quiche_config_set_initial_max_data(impl_->config, 1048576);
    quiche_config_set_initial_max_stream_data_bidi_local(impl_->config, 262144);
    quiche_config_set_initial_max_stream_data_bidi_remote(impl_->config, 262144);
    quiche_config_set_initial_max_streams_bidi(impl_->config, 100);
    quiche_config_set_initial_max_streams_uni(impl_->config, 100); // QPACK/控制流需要

    if (!ensure_cert(cfg_.cert_file, cfg_.key_file))
    {
        fprintf(stderr, "[server] 证书生成失败\n");
        running_ = false;
        return;
    }
    if (quiche_config_load_cert_chain_from_pem_file(impl_->config, cfg_.cert_file.c_str()) < 0 ||
        quiche_config_load_priv_key_from_pem_file(impl_->config, cfg_.key_file.c_str()) < 0)
    {
        fprintf(stderr, "[server] 证书加载失败\n");
        running_ = false;
        return;
    }

    fprintf(stderr, "[server] QUIC/HTTP3（quiche）运行在 127.0.0.1:%d\n", cfg_.port);

    // 事件循环
    std::vector<pollfd> pfds(1);
    pfds[0].fd = impl_->fd;
    pfds[0].events = POLLIN;
    uint8_t buf[65536];

    while (running_ && impl_->running)
    {
        // main 注册的钩子：flush 事件队列到推送流（agent 线程 emit → 事件循环投递）
        if (impl_->cbs.on_tick) impl_->cbs.on_tick();

        int rv = poll(pfds.data(), pfds.size(), 1000);
        if (rv < 0)
        {
            if (errno == EINTR) continue;
            break;
        }

        if (pfds[0].revents & POLLIN)
        {
            sockaddr_storage peer_addr{};
            socklen_t peer_len = sizeof(peer_addr);
            ssize_t n = recvfrom(impl_->fd, buf, sizeof(buf), 0, (sockaddr *)&peer_addr, &peer_len);
            if (n <= 0) continue;

            // 解析 QUIC 头获取连接 ID
            uint8_t type;
            uint32_t version;
            uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
            size_t scid_len = sizeof(scid);
            uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
            size_t dcid_len = sizeof(dcid);
            uint8_t token[256];
            size_t token_len = sizeof(token);

            int rc = quiche_header_info(buf, n, QUICHE_MAX_CONN_ID_LEN, &version, &type,
                                        scid, &scid_len, dcid, &dcid_len,
                                        token, &token_len);
            if (rc < 0)
            {
                fprintf(stderr, "[server] header_info 失败 rc=%d len=%zd\n", rc, n);
                continue;
            }

            // 查找或创建连接：按 dcid（= 服务端 SCID）查找，对齐官方示例
            uint64_t dcid_val = 0;
            memcpy(&dcid_val, dcid, dcid_len < 8 ? dcid_len : 8);
            auto it = impl_->conns.find(dcid_val);
            if (it == impl_->conns.end())
            {
                // quiche_accept：服务端创建连接的正确 API（内部处理 TLS/握手）
                QuicConn c;
                c.peer = peer_addr;
                c.peer_len = peer_len;
                c.scid = dcid_val;
                c.conn = quiche_accept(dcid, dcid_len, NULL, 0,
                                       (sockaddr *)&addr, sizeof(addr),
                                       (sockaddr *)&peer_addr, peer_len,
                                       impl_->config);
                if (!c.conn)
                {
                    fprintf(stderr, "[server] 连接创建失败\n");
                    continue;
                }
                it = impl_->conns.emplace(dcid_val, std::move(c)).first;
            }
            auto &c = it->second;

            // 喂包给引擎
            quiche_recv_info recv_info = {
                .from = (struct sockaddr *)&peer_addr,
                .from_len = peer_len,
                .to = (struct sockaddr *)&addr,
                .to_len = sizeof(addr),
            };
            ssize_t done = quiche_conn_recv(c.conn, buf, n, &recv_info);
            if (done < 0) continue;

            // 握手完成后创建 h3 连接
            if (quiche_conn_is_established(c.conn) && !c.h3)
            {
                const uint8_t *proto;
                size_t proto_len;
                quiche_conn_application_proto(c.conn, &proto, &proto_len);
                if (proto && proto_len >= 2 && memcmp(proto, "h3", 2) == 0)
                {
                    c.h3 = quiche_h3_conn_new_with_transport(c.conn, impl_->h3_config);
                }
            }

            // 处理 h3 事件
            if (c.h3)
            {
                quiche_h3_event *ev;
                int64_t stream_id;
                int poll_count = 0;
                while ((stream_id = quiche_h3_conn_poll(c.h3, c.conn, &ev)) >= 0)
                {
                    auto t = quiche_h3_event_type(ev);
                    ++poll_count;
                    const uint64_t sid = (uint64_t)stream_id;
                    if (t == QUICHE_H3_EVENT_HEADERS)
                    {
                        QuicRequest &req = c.requests[sid];
                        req = QuicRequest{};
                        quiche_h3_event_for_each_header(ev, [](uint8_t *name, size_t namelen, uint8_t *value, size_t valuelen, void *argp) {
                            auto* r = static_cast<QuicRequest*>(argp);
                            if (namelen == 7 && memcmp(name, ":method", 7) == 0)
                                r->method.assign((const char*)value, valuelen);
                            if (namelen == 5 && memcmp(name, ":path", 5) == 0)
                                r->path.assign((const char*)value, valuelen);
                            return 0; }, &req);
                    }
                    else if (t == QUICHE_H3_EVENT_DATA)
                    {
                        // 读取 body，追加到本流自己的请求上
                        QuicRequest &req = c.requests[sid];
                        uint8_t body_buf[65536];
                        ssize_t body_n;
                        while ((body_n = quiche_h3_recv_body(c.h3, c.conn, stream_id,
                                                             body_buf, sizeof(body_buf))) > 0)
                        {
                            req.body.append((const char *)body_buf, body_n);
                        }
                    }
                    else if (t == QUICHE_H3_EVENT_FINISHED)
                    {
                        const QuicRequest req = std::move(c.requests[sid]);
                        c.requests.erase(sid); // 请求到此结束，状态不留过夜
                        // 处理请求
                        if (req.method == "GET" && req.path.rfind("/events", 0) == 0)
                        {
                            // 路径里的 client_id：`GET /events?client_id=X`。
                            // 只取一个查询参数，不值得为它写个 URL 解析器
                            if (const size_t q = req.path.find("?client_id="); q != std::string::npos)
                                c.client_id = req.path.substr(q + 11);
                            if (impl_->cbs.on_client_here && !c.client_id.empty())
                                impl_->cbs.on_client_here(c.client_id);
                            // 事件推送流订阅：响应 200（fin=false），持续推送 agent 事件
                            if (!c.events_subscribed) clients_.fetch_add(1);
                            c.events_subscribed = true;
                            c.events_stream = stream_id;
                            quiche_h3_header ev_headers[] = {
                                {(uint8_t *)":status", 7, (uint8_t *)"200", 3},
                                {(uint8_t *)"content-type", 12, (uint8_t *)"text/event-stream", 17},
                            };
                            quiche_h3_send_response(c.h3, c.conn, stream_id,
                                                    ev_headers, 2, false);
                        }
                        else if (req.method == "GET" && req.path == "/commands")
                        {
                            // 斜杠命令列表（TUI 菜单数据源）。core 持有唯一真相，TUI 只渲染。
                            send_json(c, sid, impl_->cbs.on_commands ? impl_->cbs.on_commands(req.body) : "[]");
                        }
                        else if (req.method == "GET" && req.path == "/statusline")
                        {
                            // 状态栏数据（TUI 输入框下方那条，见 PROTOCOL.md）
                            send_json(c, sid, impl_->cbs.on_statusline ? impl_->cbs.on_statusline() : "{}");
                        }
                        else if (req.method == "POST" && req.path == "/interrupt")
                        {
                            if (impl_->cbs.on_interrupt) impl_->cbs.on_interrupt(req.body);
                            send_json(c, sid, "{\"status\":\"ok\"}");
                        }
                        else if (req.method == "POST" && req.path == "/agent")
                        {
                            // 建一个 agent（体 {"workdir"}），workdir 必传（ADR-0019）
                            send_json(c, sid, impl_->cbs.on_agent ? impl_->cbs.on_agent(req.body) : "{\"error\":\"no agent handler\"}");
                        }
                        else if (req.method == "POST" && req.path == "/group/close")
                        {
                            send_json(c, sid, impl_->cbs.on_group_close ? impl_->cbs.on_group_close(req.body) : "{\"ok\":true}");
                        }
                        else if (req.method == "GET" && req.path == "/agents")
                        {
                            send_json(c, sid, impl_->cbs.on_agents ? impl_->cbs.on_agents(req.body) : "[]");
                        }
                        else if (req.method == "GET" && (req.path == "/session" || req.path == "/history"))
                        {
                            send_json(c, sid, impl_->cbs.on_session_get ? impl_->cbs.on_session_get(req.body) : "[]");
                        }
                        else if (req.method == "POST" && req.path == "/message")
                        {
                            send_json(c, sid, impl_->cbs.on_message ? impl_->cbs.on_message(req.body) : "{\"status\":\"ok\"}");
                        }
                        else if (req.method == "POST" && req.path == "/command")
                        {
                            // 斜杠命令（体 {"command":"/new"}）。与 /message 的 '/' 前缀
                            // 分支共用 core 侧同一份实现，两个端点行为一致。
                            send_json(c, sid, impl_->cbs.on_command ? impl_->cbs.on_command(req.body) : "{\"error\":\"no command handler\"}");
                        }
                        else if (req.method == "GET" && req.path == "/sessions")
                        {
                            send_json(c, sid, impl_->cbs.on_sessions ? impl_->cbs.on_sessions(req.body) : "[]");
                        }
                        else if (req.method == "POST" && req.path == "/session")
                        {
                            // 体带 id = 恢复，体空 = 新建
                            send_json(c, sid, impl_->cbs.on_session ? impl_->cbs.on_session(req.body) : "{\"error\":\"no session handler\"}");
                        }
                        else if (req.method == "POST" && req.path == "/approval-response")
                        {
                            // 审批裁决回传（PROTOCOL.md）→ 审批协调器
                            std::string resp_body = "{\"error\":\"no approval handler\"}";
                            if (impl_->cbs.on_approval_response)
                            {
                                const nlohmann::json b =
                                    nlohmann::json::parse(req.body, nullptr, false);
                                const auto id = b.find("id");
                                impl_->cbs.on_approval_response(
                                    id != b.end() && id->is_string() ? id->get<std::string>()
                                                                     : std::string(),
                                    b.value("allow", false));
                                resp_body = "{\"status\":\"ok\"}";
                            }
                            send_json(c, sid, resp_body);
                        }
                        else
                        {
                            send_json(c, sid, "{\"error\":\"not found\"}", "404");
                        }
                    }
                    quiche_h3_event_free(ev);
                }
            }

            // 发送（无论是否 established——握手包也要发）
            uint8_t out[65536];
            quiche_send_info send_info{};
            ssize_t sent = quiche_conn_send(c.conn, out, sizeof(out), &send_info);
            if (sent > 0) sendto(impl_->fd, out, sent, 0, (sockaddr *)&c.peer, c.peer_len);
        }

        // 所有连接：定时器 + 发送挂起数据
        for (auto it = impl_->conns.begin(); it != impl_->conns.end();)
        {
            auto &c = it->second;
            // 走一遍 quiche 的定时器。**不叫它，max_idle_timeout 就永远不会到期**：
            // 超时是 quiche 内部记的一个时刻，它不会自己醒过来，得有人来问。
            // 客户端拔网线之后连接一直停在"活着"，于是 on_client_gone 一次都不发，
            // ADR-0021 那张 60 秒的表永远是空的
            quiche_conn_on_timeout(c.conn);
            if (quiche_conn_is_closed(c.conn))
            {
                // 承载推送流的那条连接没了 = 这个客户端没了（ADR-0021）
                if (c.events_subscribed) clients_.fetch_sub(1);
                if (impl_->cbs.on_client_gone && !c.client_id.empty())
                    impl_->cbs.on_client_gone(c.client_id);
                if (c.h3)
                {
                    quiche_h3_conn_free(c.h3);
                    c.h3 = nullptr;
                }
                quiche_conn_free(c.conn);
                c.conn = nullptr;
                it = impl_->conns.erase(it);
                continue;
            }
            uint8_t out[65536];
            quiche_send_info send_info{};
            ssize_t sent = quiche_conn_send(c.conn, out, sizeof(out), &send_info);
            if (sent > 0) sendto(impl_->fd, out, sent, 0, (sockaddr *)&c.peer, c.peer_len);
            ++it;
        }
    }
    impl_->cleanup();
    running_ = false;
    fprintf(stderr, "[server] 已停止\n");
}

} // namespace realagent