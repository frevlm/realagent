/*
 * quic_server.hpp — QUIC/HTTP3 服务端（quiche），单线程事件循环
 *
 * 普通请求按 "方法 路径" 查路由表，处理函数收请求体、回 JSON 文本。
 * `GET /events` 是推送流：长生命周期，push_event 往所有订阅者上写 SSE 帧。
 * 端点全表见 docs/PROTOCOL.md。
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace realagent {

struct QuicServerConfig {
    uint16_t port = 12345;
    std::string cert_file;
    std::string key_file;
};

/* 请求体 → JSON 响应文本。在事件循环线程上调用，不许阻塞。 */
using Handler = std::function<std::string(const std::string &body)>;

class QuicServer {
  public:
    explicit QuicServer(QuicServerConfig cfg);
    ~QuicServer();
    QuicServer(const QuicServer &) = delete;
    QuicServer &operator=(const QuicServer &) = delete;

    /* 注册 `method path`，例如 route("POST", "/message", ...)。没注册的回 404。 */
    void route(const std::string &method, const std::string &path, Handler h);
    /* 事件循环每圈调一次（main 在这里把事件队列 flush 到推送流）。 */
    void on_tick(std::function<void()> f);

    /* 往所有推送流写一帧。只在事件循环线程上调用。 */
    void push_event(const std::string &type, const std::string &payload);

    /* 阻塞跑事件循环。 */
    void run();

    /* 此刻有没有客户端订阅着推送流。任意线程可读。 */
    bool has_client() const { return clients_.load() > 0; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<int> clients_{0};
};

} // namespace realagent
