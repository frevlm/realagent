/*
 * mcp.hpp — MCP 客户端：起一个外部进程，往管道里转发（ADR-0023）
 *
 * 只说 2026-07-28。那一版起 MCP 无状态：没有 initialize、没有会话，每个请求在
 * `_meta` 里自带版本与能力。于是连上一个 server 只有两步：起进程、直接发 tools/list。
 * **一个客户端能力都不声明**（`clientCapabilities` 是个空对象）——规范禁止 server
 * 索取客户端没声明的东西，于是 MRTR（server 反过来问客户端）那条链路不存在。
 *
 * 线程模型（被逼出来的，不是选型）：连接是进程级共享的，两个 agent 会同时调同一个
 * server（规范明说允许在一条传输上交错无关请求）。于是：
 *   - 一个连接一条读线程。调用方自己读的话，读到的可能是别人的回复
 *   - **按 JSON-RPC id 认领，不按到达顺序**。响应和通知共用同一条 stdout，
 *     "有东西回来了"不等于"我的问题被回答了"
 *   - 调用方阻塞在条件变量上。agent 线程本来就顺序执行工具，阻塞是对的
 *
 * 读线程不需要 poll + 自管道：它唯一要醒来的时机是进程没了，而那时 stdout 到 EOF，
 * 它自己会退。中止不是叫醒读线程，是让**等的那一方**放弃（并按规范发一条
 * notifications/cancelled）——server 进程不能杀，杀了会把别的 agent 一起弄断。
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

#include "json.hpp"

namespace realagent {

/* 本客户端说的那一版，也是每个请求 `_meta` 里写的那个字符串 */
inline constexpr const char *kMcpProtocolVersion = "2026-07-28";

/* 一个 server 的启动规格：一份归一过的 JSON，三个键（hub.cpp 的 parse_entry 归一后去掉 name）。
 *
 *     {"command": "npx", "args": [...], "env": {...}}
 *
 * 这份对象自己就是**连接的键**（dump 出来比一比）。
 *
 * **名字不在里面。** 进程由「要 exec 什么」决定，不由它叫什么决定：同一份命令在两个
 * [[plugin]] 里各配了一次（现成 plugin 里很常见），起一个进程就够，两个名字挂在租约上。
 * 名字是配置给的标签，连接不该知道它。 */

/* 一个连接。构造即连上（起进程 + tools/list），构造失败返回空指针。 */
class McpClient {
  public:
    /* 起进程并拉一次工具清单。失败返回 nullptr 并写 err（原话，给 stderr 用）。
     * 清单在这里定死，一趟中间不重拉——清单变形等于每轮 prompt cache 重来。 */
    static std::unique_ptr<McpClient> start(const nlohmann::json &cfg, std::string &err);

    ~McpClient();

    McpClient(const McpClient &) = delete;
    McpClient &operator=(const McpClient &) = delete;

    /* 那份规格本身，dump 出来就是连接的键。 */
    const nlohmann::json &cfg() const { return cfg_; }

    /* tools/list 拉回来的原始工具对象数组（分页已经跟完）。
     * core 不原样收下它——从每个对象里取 name / description / inputSchema，其余自己写。 */
    const nlohmann::json &tools() const { return tools_; }

    /* 调一个工具。name 是 server 那头的原名（不带前缀）。
     *
     * 返回 MCP 的 result 对象（`{"content": [...], "isError": bool}`）。失败时返回一个
     * 自造的同形对象：isError=true、content 里一个 text 块装人话——协议错误与工具自己
     * 报的错走同一条路（ADR-0023），模型改不了协议错误，但它必须知道这条路走不通。
     *
     * abort 非空且变真时：按规范发 notifications/cancelled，**无视迟到的响应**，
     * 立刻返回。不杀进程。 */
    nlohmann::json call(const std::string &name, const nlohmann::json &arguments,
                        const std::atomic<bool> *abort = nullptr);

  private:
    McpClient() = default;

    /* 发一个请求，等回一个**已经确认过的 complete result**（不是原始响应）。
     * 失败返回 nullopt，人话原因写进 err——**原因由这里说**：调用方再猜一次读的是同一个
     * 原子量的另一个时刻，超时之后按下的 Esc 会被报成「中止」。 */
    std::optional<nlohmann::json> request(const std::string &method, nlohmann::json params,
                                          const std::atomic<bool> *abort, int timeout_ms,
                                          std::string &err);
    void send_line(const nlohmann::json &msg);
    void reader_loop();
    void shutdown();

    nlohmann::json cfg_;
    nlohmann::json tools_ = nlohmann::json::array();

    pid_t pid_ = -1;
    int in_fd_ = -1;  // 写到 server 的 stdin
    int out_fd_ = -1; // 从 server 的 stdout 读
    std::thread reader_;

    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<uint64_t> next_id_{1};
    std::map<uint64_t, nlohmann::json> done_; // id → 响应，读线程填，调用方取走
    bool closed_ = false;                     // stdout 到 EOF：等的人都该醒来

    std::mutex write_mtx_; // 一行不许被另一行插进去
};

/* 走一遍扫描链（plugin_roots()，plugin.hpp / ADR-0024），合并成一张启动规格表。
 * 远的在前、近的在后，同名条目被近的那份整条换掉——顺序由那一处给，这里只顺着走。
 *
 * **同名整条覆盖，不逐字段合并**：一条启动规格是一束共变的东西（命令、参数、环境），
 * 拆开合并能拼出一个谁都没写过的启动命令（同 ADR-0017 拒绝把端点束拆成四个开关）。
 *
 * 文件不存在不是错。**未知键一律忽略**（`timeout` / `cwd` / `enabled_tools` / `headers`
 * 全部读过即弃，归一时落下）——先例是配置合并「只认默认树里有的键」；副产品是 JSON 有了注释（`"//"`）。
 * 坏条目跳过，原话写进 errors，core 照常起（`models.json` 那条先例）。 */
struct McpConfig {
    std::vector<nlohmann::json> servers; // 已合并、已归一、已按名字排序
    std::vector<std::string> errors;     // 跳过的那些，人话
};
/* bridge 是 `mcp_http_bridge` 那个模板（配置里的一个真默认值）：
 * `type: "http"` 的条目在归一那一层就靠它变成一条 stdio 规格（ADR-0024 §5），
 * 于是下游拿到的东西一模一样——连接的键、去重、生死、转发，一律不知道它原来是 http。
 * 空数组 = 不支持 http，遇到报一条人话。 */
McpConfig load_mcp_config(const std::string &workdir, const nlohmann::json &bridge);

/* 进程级的连接池。**一份配置一个连接**，全部组、全部 agent、全部 workdir 共用——
 * 协议把工作区身份整个移出了连接（ADR-0023 §4），按目录复制拿不到任何不同的东西。
 *
 * 生死跟着组：组建起来时 open() 一次，Lease 活着连接就在，最后一个 Lease 没了连接就关。
 * **引用计数不自己写**：池里存 weak_ptr，Lease 拿 shared_ptr——最后一个走了，
 * McpClient 析构，进程收尾。少一个计数器，也就少一个对不上的机会。 */
class McpHub {
  public:
    /* 一个组手里的那份东西。析构 = 松手。 */
    struct Lease {
        /* 一个名字配一个连接。**同一个连接可以挂着两个名字**——名字是配置给的标签，
         * 进程由命令决定。工具名照旧靠各自的名字分开，共用进程不等于共用名字。 */
        struct Conn {
            std::string name; // `<plugin 前缀>_<配置的键>`，前缀为空时就是那个键
            std::shared_ptr<McpClient> client;
        };
        std::vector<Conn> conns;
        /* 直接能拼进 dialog["tools"] 的形状，外加一个 `_core` 键装 core 私有的字段
         * （发出去之前 erase 掉那一个键就行）。名字已经是 `<前缀>_<键>__<原名>`。 */
        nlohmann::json tools = nlohmann::json::array();
        std::vector<std::string> errors; // 没连上的那些，原话进 stderr
    };

    /* 按 workdir 读配置、把还没连的连上（并行）、拼出工具表。
     * 已经有人连着的同一份配置直接复用，不再起第二个进程。 */
    Lease open(const std::string &workdir, const nlohmann::json &bridge);

  private:
    std::mutex mtx_;
    std::map<std::string, std::weak_ptr<McpClient>> conns_; // identity → 连接
};

} // namespace realagent
