/*
 * mcp.hpp — MCP 客户端与连接池（ADR-0023）
 *
 * 只说 2026-07-28 版：无状态，没有 initialize；连上就是起进程、发 tools/list。
 * 客户端不声明任何能力，于是 server 不会反过来问客户端。
 *
 * 连接是进程级共享的，多个 agent 会同时调同一个 server：一个连接一条读线程，
 * 响应按 JSON-RPC id 认领，调用方阻塞在条件变量上等自己那一条。
 * 中断时调用方放弃等待并发 notifications/cancelled，不杀进程（别的 agent 还在用）。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <realetting/json.hpp>

namespace realagent {

inline constexpr const char *kMcpProtocolVersion = "2026-07-28";

/* 一个连接。启动规格是归一过的 `{"command", "args", "env"}`，它 dump 出来就是连接的键：
 * 进程由「要 exec 什么」决定，不由配置里叫什么决定。 */
class McpClient {
  public:
    /* 起进程并拉一次工具清单（之后不再重拉）。失败返回 nullptr 并写 err。 */
    static std::unique_ptr<McpClient> start(const nlohmann::json &cfg, std::string &err);
    ~McpClient();
    McpClient(const McpClient &) = delete;
    McpClient &operator=(const McpClient &) = delete;

    const nlohmann::json &cfg() const { return cfg_; }
    /* tools/list 的原始工具对象数组（分页已跟完）。 */
    const nlohmann::json &tools() const { return tools_; }

    /* 调一个工具（name 是 server 那头的原名）。返回 `{"content", "isError"}`；
     * 协议层失败也造一个同形的 isError 结果。abort 变真时立刻返回。 */
    nlohmann::json call(const std::string &name, const nlohmann::json &arguments,
                        const std::atomic<bool> *abort = nullptr);

  private:
    McpClient() = default;

    /* 发请求、等到一个确认过的 complete result。失败返回 nullopt，原因写进 err。 */
    std::optional<nlohmann::json> request(const std::string &method, nlohmann::json params,
                                          const std::atomic<bool> *abort, int timeout_ms,
                                          std::string &err);
    void send_line(const nlohmann::json &msg);
    void reader_loop();
    void shutdown();

    nlohmann::json cfg_;
    nlohmann::json tools_ = nlohmann::json::array();

    pid_t pid_ = -1;
    int in_fd_ = -1;  // server 的 stdin
    int out_fd_ = -1; // server 的 stdout
    std::thread reader_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<uint64_t> next_id_{1};
    std::map<uint64_t, nlohmann::json> done_; // id → 响应
    bool closed_ = false;                     // stdout 到 EOF

    std::mutex write_mtx_; // 一行不许被另一行插进去
};

/* 扫描链上全部 mcp.json 合并后的启动规格表。同名条目被近的整条换掉（不逐字段合并）；
 * 未知键忽略；坏条目跳过，原因进 errors。 */
struct McpConfig {
    std::vector<nlohmann::json> servers; // 每条 {"name", "command", "args", "env"}，按名排序
    std::vector<std::string> errors;
};
/* bridge 是配置项 mcp_http_bridge：`type: "http"` 的条目靠它变成一条 stdio 规格
 * （ADR-0024 §5），下游不知道它原来是 http。空数组 = 不支持 http。 */
McpConfig load_mcp_config(const std::string &workdir, const nlohmann::json &bridge);

/* 进程级连接池：一份规格一个连接，全部 agent 共用。池里存 weak_ptr，Lease 持 shared_ptr，
 * 最后一个 Lease 析构时进程收尾。 */
class McpHub {
  public:
    struct Lease {
        /* 同一个连接可以挂两个名字（两个 plugin 配了同一条命令）。 */
        struct Conn {
            std::string name; // `<plugin 前缀>_<配置的键>`
            std::shared_ptr<McpClient> client;
        };
        std::vector<Conn> conns;
        /* 可直接拼进 dialog["tools"]，外加 `_core`。名字是 `<conn 名>__<原名>`。 */
        nlohmann::json tools = nlohmann::json::array();
        std::vector<std::string> errors;
    };

    /* 读配置、并行连上还没连的、拼工具表。已经连着的直接复用。 */
    Lease open(const std::string &workdir, const nlohmann::json &bridge);

  private:
    std::mutex mtx_;
    std::map<std::string, std::weak_ptr<McpClient>> conns_; // 规格 dump → 连接
};

} // namespace realagent
