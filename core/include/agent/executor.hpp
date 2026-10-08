/*
 * executor.hpp — 一个 agent 的工具执行器
 *
 * 查定义 → 危险工具按 permission 裁决（ask 走审批）→ PreToolUse hook → 执行
 * → PostToolUse hook。单 agent 内工具严格顺序执行。
 *
 * 中断就是一个标志：interrupt() 从事件循环线程置位，正在跑的 bash / MCP / hook
 * 轮询它自己停下，之后的 execute 直接拒绝，直到 reset()。
 */
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "agent/approval.hpp"
#include "agent/catalog.hpp"
#include "agent/context.hpp"
#include "agent/hooks.hpp"
#include "mcp/mcp.hpp"
#include "tools/tools.hpp"

namespace realagent {

class Agents;

class Executor {
  public:
    /* pool / agent_id：spawn 与 send_message 要用；pool 为空时这两个工具报错。
     * mcp / hooks / defs 归 Agent 所有，这里只读；为空就是没有。 */
    Executor(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir,
             Agents *pool = nullptr, int agent_id = 0, const McpHub::Lease *mcp = nullptr,
             const Hooks *hooks = nullptr, const std::vector<AgentDef> *defs = nullptr);

    /* 先内置，再 MCP（MCP 的名字带前缀，撞不上内置）。没有返回 nullptr。 */
    const nlohmann::json *find(const std::string &name) const;

    bool check_permission(const nlohmann::json &tool, const std::string &params_json,
                          std::string *denied_reason);

    /* 返回 `{"content", "isError", "interrupted"}`，三个字段每条路径都写齐。 */
    nlohmann::json execute(const std::string &call_id, const std::string &name,
                           const std::string &params_json);

    /* 连同这个 agent 挂着的审批一起按 deny 掐掉：不然中断要干等 30 秒超时。 */
    void interrupt()
    {
        interrupted_ = true;
        approval_.cancel(agent_id_);
    }
    void reset() { interrupted_ = false; }

  private:
    nlohmann::json agent_tool(const std::string &name, const nlohmann::json &params);
    nlohmann::json mcp_call(const nlohmann::json &tool, const nlohmann::json &params);

    CoreContext &ctx_;
    ApprovalCoordinator &approval_;
    std::string workdir_;
    Agents *pool_ = nullptr;
    int agent_id_ = 0;
    const McpHub::Lease *mcp_ = nullptr;
    const Hooks *hooks_ = nullptr;
    const std::vector<AgentDef> *defs_ = nullptr;
    std::atomic<bool> interrupted_{false};
};

} // namespace realagent
