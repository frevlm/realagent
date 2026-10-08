/*
 * server.cpp — HTTP 路由 + WebSocket 推送
 */
#include "server/server.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace realagent {

namespace {

// 能连 core 的网页只有 gui 的 Wails 页面：窗口（macOS / Linux、Windows）、`wails dev` 下的窗口、
// `wails dev` 给浏览器开的那个端口。不带 Origin 的是 TUI、curl 这类非浏览器客户端，放行；
// 别的源一律 403——core 没有认证，放进一个网站就等于让它替用户跑 bash（ADR-0028）
constexpr std::array ORIGINS{"wails://wails", "http://wails.localhost", "wails://wails.localhost:34115",
                             "http://localhost:34115"};

httplib::Server::HandlerResponse check_origin(const httplib::Request &req, httplib::Response &res)
{
    if (!req.has_header("Origin")) return httplib::Server::HandlerResponse::Unhandled;
    const std::string origin = req.get_header_value("Origin");
    if (std::find(ORIGINS.begin(), ORIGINS.end(), origin) == ORIGINS.end())
    {
        res.status = 403;
        return httplib::Server::HandlerResponse::Handled;
    }
    res.set_header("Access-Control-Allow-Origin", origin);
    if (req.method != "OPTIONS") return httplib::Server::HandlerResponse::Unhandled;
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.status = 204;
    return httplib::Server::HandlerResponse::Handled;
}

} // namespace

void Server::route(const std::string &method, const std::string &path, Handler h)
{
    auto serve = [this, h = std::move(h)](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lk(req_mtx_);
        // 浏览器发不出带体的 GET，同一份 JSON 改放查询串 ?body=
        const std::string body = req.body.empty() ? req.get_param_value("body") : req.body;
        res.set_content(h(body), "application/json");
        if (after_) after_();
    };
    if (method == "GET")
        http_.Get(path, serve);
    else
        http_.Post(path, serve);
}

void Server::push_event(const std::string &type, const std::string &payload)
{
    const std::string frame = R"({"event":")" + type + R"(","data":)" + payload + "}";
    std::lock_guard<std::mutex> lk(conns_mtx_);
    for (httplib::ws::WebSocket *ws : conns_) ws->send(frame);
}

bool Server::has_client()
{
    std::lock_guard<std::mutex> lk(conns_mtx_);
    return !conns_.empty();
}

bool Server::run(int port)
{
    http_.WebSocket("/events", [this](const httplib::Request &, httplib::ws::WebSocket &ws) {
        // 不设的话 300 秒收不到客户端的帧就断，而客户端从不发帧；它走了，读自己会失败
        ws.set_read_timeout(0);
        {
            std::lock_guard<std::mutex> lk(conns_mtx_);
            conns_.insert(&ws);
        }
        for (std::string msg; ws.read(msg) != httplib::ws::Fail;)
        {
        }
        std::lock_guard<std::mutex> lk(conns_mtx_);
        conns_.erase(&ws);
    });
    // 在 WebSocket 升级之前也跑
    http_.set_pre_routing_handler(check_origin);
    // 增量帧只有几十字节，不能让 Nagle 攒着
    http_.set_tcp_nodelay(true);
    // 一台机器一个 core：端口就是那把锁。httplib 默认的 SO_REUSEPORT 会让第二个 core 也绑上
    http_.set_socket_options([](socket_t s) { httplib::set_socket_opt(s, SOL_SOCKET, SO_REUSEADDR, 1); });
    if (!http_.bind_to_port("127.0.0.1", port))
    {
        fprintf(stderr, "[server] 127.0.0.1:%d 被占用——core 已在运行？\n", port);
        return false;
    }
    fprintf(stderr, "[server] 运行在 127.0.0.1:%d\n", port);
    return http_.listen_after_bind();
}

} // namespace realagent
