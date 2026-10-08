/*
 * server.hpp — core 的对外服务（cpp-httplib），只听 127.0.0.1
 *
 * 请求是普通 HTTP：按 "方法 路径" 注册，处理函数收请求体、回 JSON 文本。
 * 推送走 WebSocket `GET /events?client_id=`：连接按 client_id 认组（ADR-0021）。
 * 端点全表见 docs/PROTOCOL.md。
 */
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

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
    /* 只往 client 那一组的连接写。任意线程可调。 */
    void push_to(const std::string &client, const std::string &type, const std::string &payload);
    /* 此刻 client 有没有连着推送。任意线程可调。 */
    bool has_client(const std::string &client);
    /* 断线满 age 还没回来的 client，取走即忘。任意线程可调。 */
    std::vector<std::string> gone_for(std::chrono::seconds age);
    /* 阻塞监听。端口被占（core 已在跑）就返回 false。 */
    bool run(int port);

  private:
    httplib::Server http_;
    std::mutex req_mtx_;
    std::mutex conns_mtx_;
    std::map<httplib::ws::WebSocket *, std::string> conns_;             // → client_id
    std::map<std::string, std::chrono::steady_clock::time_point> left_; // 最后一条连接断开的时刻
    std::function<void()> after_;
};

} // namespace realagent
