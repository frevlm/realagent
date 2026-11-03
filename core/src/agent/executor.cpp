#include "agent/executor.hpp"

#include "agent/agents.hpp"

#include <chrono>
#include <cstdio>
#include <future>
#include <utility>

namespace realagent {

Executor::Executor(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir,
                   Agents *pool, int agent_id, const McpHub::Lease *mcp,
                   const Hooks *hooks, const std::vector<AgentDef> *defs)
    : ctx_(ctx), approval_(approval), workdir_(std::move(workdir)), pool_(pool),
      agent_id_(agent_id), mcp_(mcp), hooks_(hooks), defs_(defs)
{
}

const nlohmann::json *Executor::find(const std::string &name) const
{
    if (const nlohmann::json *t = find_tool(name)) return t;
    return mcp_ ? find_by_name(mcp_->tools, name) : nullptr;
}

/* 一个实现服务全部 MCP 工具：它们是 N 份声明，不是 N 段代码。 */
nlohmann::json Executor::mcp_call(const nlohmann::json &tool, const nlohmann::json &params)
{
    const std::string server = tool_server(tool);
    for (const auto &c : mcp_->conns)
        if (c.name == server)
            return c.client->call(tool["_core"]["remote_name"], params, &interrupted_);
    return tool_fail("MCP server 不在手上: " + server);
}

namespace {
/* permission 配置：ask（默认）/ allow-all / deny。认不出的值按 ask。 */
Verdict decide(const Config &cfg)
{
    const std::string mode = cfg.get("permission");
    if (mode == "allow-all") return Verdict::Allow;
    if (mode == "deny") return Verdict::Deny;
    if (mode != "ask" && !mode.empty())
        fprintf(stderr, "[perm] 未知 permission=%s，按 ask 处理\n", mode.c_str());
    return Verdict::Ask;
}
} // namespace

bool Executor::check_permission(const nlohmann::json &tool, const std::string &params_json,
                                std::string *denied_reason)
{
    if (!tool_dangerous(tool)) return true;
    const auto deny = [denied_reason](const char *why) {
        if (denied_reason) *denied_reason = why;
        return false;
    };
    switch (decide(*ctx_.config))
    {
        case Verdict::Allow:
            return true;
        case Verdict::Deny:
            return deny("denied by permission policy");
        case Verdict::Ask:
            // 没有客户端就没人能裁决：当场拒，不空等 30 秒超时
            if (ctx_.online && !ctx_.online()) return deny("无客户端可裁决");
            if (approval_.await(agent_id_, tool.value("name", std::string()), params_json,
                                ctx_.emit_fn) != Verdict::Allow)
                return deny("denied by user");
            return true;
    }
    return false;
}

nlohmann::json Executor::execute(const std::string &call_id, const std::string &name,
                                 const std::string &params_json)
{
    const auto fail = [](const std::string &why, bool intr) {
        nlohmann::json r = tool_fail(why);
        r["interrupted"] = intr;
        return r;
    };

    const nlohmann::json *tool = find(name);
    if (!tool) return fail("unknown tool", false);

    std::string reason;
    if (!check_permission(*tool, params_json, &reason)) return fail(reason, false);

    // PreToolUse 排在 permission 之后：hook 只能收紧，不能把 ask 变成 allow
    nlohmann::json hp{{"matcher_key", name}, {"agent_id", agent_id_}, {"cwd", workdir_}, {"tool_name", name}, {"tool_input", params_json}};
    if (hooks_)
        if (const HookOutcome h = hooks_->run(HookEvent::PreToolUse, hp, &interrupted_); h.deny)
            return fail(h.reason.empty() ? "被 hook 拦下" : h.reason, false);

    if (interrupted_) return fail("interrupted by user", true);

    nlohmann::json params = nlohmann::json::parse(params_json, nullptr, false);
    if (params.is_discarded()) params = nlohmann::json::object();

    nlohmann::json r;
    if (!tool_server(*tool).empty())
        r = mcp_call(*tool, params);
    else if (name == "spawn")
        r = spawn(params);
    else
        r = run_tool(call_id, name, params, ctx_.emit_fn, workdir_, &interrupted_);
    r["interrupted"] = interrupted_.load();

    // PostToolUse 只在真产出了结果时跑：工具报错算，被中断不算
    if (!r["interrupted"])
    {
        hp["tool_response"] = r.value("content", nlohmann::json::array());
        if (hooks_) hooks_->run(HookEvent::PostToolUse, std::move(hp), &interrupted_);
    }
    return r;
}

nlohmann::json Executor::spawn(const nlohmann::json &params)
{
    if (!pool_) return tool_fail("spawn is not available here");
    const auto str = [&params](std::string_view k) { return tool_arg(params, k).value_or(""); };

    // `agent` 查的是写进本 agent system prompt 的那张表
    std::string def_body;
    if (const std::string want = str("agent"); !want.empty())
    {
        const AgentDef *d = nullptr;
        if (defs_)
            for (const AgentDef &x : *defs_)
                if (x.name == want) d = &x;
        if (!d) return tool_fail("unknown agent definition: " + want);
        def_body = d->body;
    }

    struct Outcome {
        std::string answer, error;
    };
    std::promise<Outcome> done;
    std::future<Outcome> got = done.get_future();
    bool fired = false; // promise 只能兑现一次
    std::string err;
    const std::string workdir = str("workdir").empty() ? workdir_ : str("workdir");
    std::shared_ptr<Agent> child = pool_->spawn(
        agent_id_, workdir, def_body,
        [&done, &fired](const std::string &answer, const std::string &error) {
            if (!std::exchange(fired, true)) done.set_value({answer, error});
        },
        err);
    if (!child) return tool_fail(err);
    child->post(str("prompt"));
    // 中断要一直往下传：子 agent 开跑时会清掉自己的中断标志，只传一次可能落空
    while (got.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
        if (interrupted_) child->interrupt();
    const std::string sid = child->session_id();
    child.reset(); // 在这里析构：join 它的线程，MCP 租约随之归还
    // 会话 id 是给客户端的：回放时它凭这个把子 agent 的过程嵌回这张工具卡片下面
    const std::string tail = "\n\n(session " + sid + ")";
    if (interrupted_) return tool_fail("interrupted by user" + tail);
    const Outcome o = got.get();
    if (!o.error.empty()) return tool_fail("the agent stopped on an error: " + o.error + tail);
    return tool_ok((o.answer.empty() ? "(the agent finished without a final message)" : o.answer) + tail);
}

} // namespace realagent
