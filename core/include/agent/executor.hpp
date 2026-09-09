/*
 * executor.hpp — 工具执行器（agent 模块）
 *
 * 链路（ADR-0002 / ADR-0005）：
 *   查工具定义 → dangerous 则按 permission 配置裁决（ASK 走审批协调器）→ 执行
 * 单 agent 内工具严格顺序执行。
 *
 * 中止（ADR-0002 R8 的后一半）：execute 卡在 bash 里时，interrupt() 从事件循环线程
 * 进来把子进程组打掉。顺序执行的语义正好意味着"在跑的工具"至多一个，
 * 连指针都不必记——工具那边一个 pid 就够。
 */
#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include "agent/agent_defs.hpp"
#include "agent/approval.hpp"
#include "agent/context.hpp"
#include "agent/hooks.hpp"
#include "mcp/mcp.hpp"
#include "tools/tools.hpp"

namespace realagent {

class Agents;

class Executor {
  public:
    /* pool 与 agent_id 是"去哪儿找别的 agent"和"谁在用我"，spawn / send_message 要这两样。
     * 独立构造（测试、test-tools）时 pool 为空，那两个工具就报"这里没有 agent 图"。
     * 进构造函数而不是事后 bind()：两段式初始化多出一个"造好了但还没接上"的中间态，
     * 而这里根本不需要它——Agent 的 pool_ / id_ 在 exe_ 之前就已经就位了。 */
    /* mcp 是这个 agent 手里那份 MCP 清单（Agent 持有，Executor 只读）。
     * 空 = 这里没有 MCP，工具表就只有内置那六个。 */
    /* defs 是这个 agent 看得见的 agent 定义（同 mcp / hooks：Agent 持有，Executor 只读）。
     * `spawn` 的 `agent` 参数就认这张表里的名字——**它与写进 system prompt 的是同一张**，
     * 所以不回头去问 pool 要：那会多出「找不到自己」这个到不了的分支。 */
    Executor(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir,
             Agents *pool = nullptr, int agent_id = 0, const McpHub::Lease *mcp = nullptr,
             const Hooks *hooks = nullptr, const std::vector<AgentDef> *defs = nullptr);

    /* 按名查定义：先内置，再 MCP。**MCP 的名字带前缀，撞不上内置那六个。**
     * 查不到返回 nullptr。 */
    const nlohmann::json *find(const std::string &name) const;

    /* 权限检查：dangerous 工具按 permission 配置裁决。ASK → 真等用户裁决（ADR-0005）。 */
    bool check_permission(const nlohmann::json &tool, const std::string &params_json,
                          std::string *denied_reason);

    /* 执行工具。返回 run_tool 的 json（{"status","output"}），再加一个 "interrupted"：
     * 本次执行期间 core 提过中止——与"工具自己失败了"不是一回事。
     * call_id 透传给工具：实时输出帧（tool_output）要靠它认领是哪次调用。 */
    nlohmann::json execute(const std::string &call_id, const std::string &name,
                           const std::string &params_json);

    /* 中止在跑的工具（任意线程）。没有在跑的也要记下——紧随其后的那次 execute 直接拒掉，
     * 否则用户按了中止、模型的下一个工具照跑不误。 */
    void interrupt();

    /* 新一轮 run 的起点：抹掉上一轮的中止痕迹。不做这一步，中止会一直粘着（同 abort_）。 */
    void reset();

  private:
    CoreContext &ctx_;
    ApprovalCoordinator &approval_;
    std::string workdir_; // 工具的相对路径从这里算起，bash 也 chdir 到这里
    Agents *pool_ = nullptr;
    int agent_id_ = 0;                   // 这个 executor 属于哪个 agent
    const McpHub::Lease *mcp_ = nullptr; // 这个 agent 看得见的 MCP 工具与连接
    /* 这个 agent 看得见的 hook（ADR-0024 §6）。空 = 一个都没装，run 立即返回，零开销。 */
    const Hooks *hooks_ = nullptr;
    /* 这个 agent 看得见的 agent 定义（ADR-0024 §8）。空 = 这里没有图，也就没有 spawn。 */
    const std::vector<AgentDef> *defs_ = nullptr;

    /* spawn / send_message：它们要认识 Agents，所以实现在这儿而不在 tools.cpp——
     * tools/ 在 agent/ 下面，反过来包含就是层级倒挂。 */
    nlohmann::json agent_tool(const std::string &name, const nlohmann::json &params);

    /* 转发给外部进程。**N 个 MCP 工具共用这一个实现**——它们不是 N 段代码，是 N 份声明。 */
    nlohmann::json mcp_call(const nlohmann::json &tool, const nlohmann::json &params);

    std::mutex inflight_mtx_;
    bool inflight_ = false;
    /* 原子而不是靠 inflight_mtx_ 护着：MCP 调用要在**另一条线程**上无锁地读它
     * （McpClient 等响应时按它决定要不要放弃）。登记/取用的顺序仍旧由那把锁保证。 */
    std::atomic<bool> interrupted_{false};
};

} // namespace realagent
