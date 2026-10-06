/*
 * server.cpp — HTTP 路由 + WebSocket 推送
 */
#include "server/server.hpp"

#include <cstdio>

namespace realagent {

void Server::route(const std::string &method, const std::string &path, Handler h)
{
    auto serve = [this, h = std::move(h)](const httplib::Request &req, httplib::Response &res) {
        std::lock_guard<std::mutex> lk(req_mtx_);
        res.set_content(h(req.body), "application/json");
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
