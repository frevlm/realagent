/*
 * server.hpp — core 的对外服务（cpp-httplib），只听 127.0.0.1
 *
 * 请求是普通 HTTP：按 "方法 路径" 注册，处理函数收请求体、回 JSON 文本。
 * 推送走 WebSocket `GET /events`：push_event 往每一条连接写一帧。
 * 端点全表见 docs/PROTOCOL.md。
 */
#pragma once

#include <functional>
#include <mutex>
#include <set>
#include <string>

#include <httplib.h>

namespace realagent {

/* 请求体 → JSON 响应文本。所有请求排成一队，一次调一个，不许阻塞。 */
using Handler = std::function<std::string(const std::string &body)>;

class Server {
  public:
    /* method 只认 "GET" / "POST"。 */
    void route(const std::string &method, const std::string &path, Handler h);
    /* 每处理完一条请求调一次，仍在那一队里（main 在这里比对状态栏载荷）。 */
    void after_request(std::function<void()> f) { after_ = std::move(f); }
    /* 往每一条推送连接写一帧。任意线程可调。 */
    void push_event(const std::string &type, const std::string &payload);
    /* 此刻有没有客户端连着推送。任意线程可调。 */
    bool has_client();
    /* 阻塞监听。端口被占（core 已在跑）就返回 false。 */
    bool run(int port);

  private:
    httplib::Server http_;
    std::mutex req_mtx_;
    std::mutex conns_mtx_;
    std::set<httplib::ws::WebSocket *> conns_;
    std::function<void()> after_;
};

} // namespace realagent
