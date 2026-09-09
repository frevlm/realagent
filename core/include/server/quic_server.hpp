/*
 * quic_server.hpp — QUIC/HTTP3 服务端（M5）
 *
 * 基于 Cloudflare quiche（QUIC + HTTP/3 一体）的最小实现：
 *  - 接受 QUIC 连接
 *  - 解析 HTTP/3 请求（端点全表见 docs/PROTOCOL.md）
 *  - 事件推送流（GET /events，一条长生命周期可靠流，SSE 语义）
 *
 * 生命周期：单线程事件循环（poll-based），通过回调与 agent 线程交互。
 */
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace realagent {

/* 服务端配置 */
struct QuicServerConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 12345;
    std::string cert_file = ".realagent/cert.pem";
    std::string key_file = ".realagent/key.pem";
    /* 最大并发连接数 */
    int max_connections = 16;
};

/* QUIC 服务端回调（agent 线程注册） */
struct QuicCallbacks {
    /* 收到用户消息 → 投递 agent 线程。返回 JSON 响应字符串。 */
    std::function<std::string(const std::string &body)> on_message;
    /* 执行斜杠命令（POST /command，体 {"command":"/new"}）→ 响应 JSON 字符串。
     * 与 on_message 的 `/` 前缀分支是同一份实现，不是两套行为。 */
    std::function<std::string(const std::string &body)> on_command;
    /* 会话清单（GET /sessions，体 {"agent_id"}）→ JSON 数组字符串。
     * 会话目录跟着 agent 的 workdir 走，所以这个 GET 也要指名道姓（ADR-0019）。 */
    std::function<std::string(const std::string &body)> on_sessions;
    /* 新建 / 恢复会话（POST /session，体 {"id"} 恢复、体空则新建）→ 响应 JSON 字符串 */
    std::function<std::string(const std::string &body)> on_session;
    /* 收到审批裁决（POST /approval-response）→ 交给审批协调器 */
    std::function<void(const std::string &id, bool allow)> on_approval_response;
    /* 斜杠命令列表（GET /commands）→ JSON 数组 [{name,description},...]。core 是唯一真相源。 */
    /* GET /commands。body 里可带 agent_id：prompt 命令表跟着 agent 的 workdir 走
     * （ADR-0024 §7）。不带就只回内置那三条——老客户端照常能用。 */
    std::function<std::string(const std::string &)> on_commands;
    /* 状态栏数据（GET /statusline）→ JSON 对象字符串 {"model":"...", ...}。
     * 与推送流的 status_update 帧（状态行，本次 run 实时数字）不是一回事 */
    std::function<std::string()> on_statusline;
    /* 中断某个 agent 的 run（POST /interrupt，体 {"agent_id"}，TUI Esc 触发）。
     * 多 agent 之后必须指名道姓，否则 Esc 一按全场停摆（ADR-0019）。 */
    std::function<void(const std::string &body)> on_interrupt;
    /* 建一个 agent（POST /agent，体 {"workdir"}）→ {"agent_id"} */
    std::function<std::string(const std::string &body)> on_agent;
    /* agent 清单（GET /agents，体 {"client_id"}）→ JSON 数组。只列调用方那一组（ADR-0021） */
    std::function<std::string(const std::string &body)> on_agents;
    /* 会话内容回放（GET /session 或 GET /history，体 {"client_id","agent_id"}）→ 事件帧数组。
     * 客户端据此把一段自己没在看的时间画出来，用的是渲染实时流那份代码（ADR-0020）。 */
    std::function<std::string(const std::string &body)> on_session_get;
    /* 每轮事件循环调用（main 把事件队列 flush 到推送流，ADR-0002 线程模型） */
    std::function<void()> on_tick;
    /* 新推送流注册（M6 接入：agent 事件推送给客户端） */
    std::function<void(uint64_t stream_id)> on_push_stream;

    /* 关掉一整组（POST /group/close，体 {"client_id"}）。客户端正常退出前显式发一次 */
    std::function<std::string(const std::string &body)> on_group_close;

    /* 客户端来了 / 走了（ADR-0021）。
     *
     * 依据是**推送流**：一个客户端一条、长生命周期，`GET /events?client_id=X` 一订阅
     * 就是"它在"，承载它的 QUIC 连接被回收就是"它没了"。连接死活由 QUIC 自己的
     * max_idle_timeout 探，应用层不自建心跳。
     *
     * gone 不等于关组：客户端可能只是网抖了一下。关不关由上层那张 60 秒的表决定。 */
    std::function<void(const std::string &client_id)> on_client_here;
    std::function<void(const std::string &client_id)> on_client_gone;
};

/* 最小 QUIC/HTTP3 服务端。事件循环阻塞在 run() 中。 */
class QuicServer {
  public:
    explicit QuicServer(const QuicServerConfig &cfg);
    ~QuicServer();

    QuicServer(const QuicServer &) = delete;
    QuicServer &operator=(const QuicServer &) = delete;

    /* 设置回调（agent 线程注册） */
    void set_callbacks(const QuicCallbacks &cbs) { cbs_ = cbs; }

    /* 推送事件到所有订阅了 /events 的客户端（agent 事件流，SSE 语义） */
    void push_event(const std::string &type, const std::string &payload);

    /* 运行事件循环（阻塞，信号退出） */
    void run();
    /* 请求停止 */
    void stop() { running_ = false; }

    bool is_running() const { return running_; }

    /* 此刻有没有客户端订阅着推送流。**agent 线程读**（审批要它：没人能裁决就当场拒绝，
     * 不等那 30 秒——ADR-0019 §8），所以是 atomic 而不是去数 conns：那张表归事件循环线程。 */
    bool has_client() const { return clients_.load() > 0; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    QuicServerConfig cfg_;
    QuicCallbacks cbs_;
    std::atomic<bool> running_{false};
    std::atomic<int> clients_{0};
};

} // namespace realagent