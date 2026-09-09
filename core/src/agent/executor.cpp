#include "agent/executor.hpp"

#include "agent/agents.hpp"

#include <cstdio>

#include "json.hpp"

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
    if (const nlohmann::json *t = find_tool(name)) return t; // 内置那六个
    return mcp_ ? find_by_name(mcp_->tools, name) : nullptr;
}

/* 转发。**这一个实现服务全部 MCP 工具**——它们不是 N 段代码，是 N 份声明（ADR-0023 §1）。 */
nlohmann::json Executor::mcp_call(const nlohmann::json &tool, const nlohmann::json &params)
{
    const std::string server = tool_server(tool);
    for (const auto &c : mcp_->conns)
        if (c.name == server)
            return c.client->call(tool["_core"]["remote_name"], params, &interrupted_);
    /* core 是常驻服务，一个空指针会带走所有 agent，所以这里不解引用一个找不到的连接。
     * 清单与连接是 hub 同一个循环里生成的，正常路径到不了这儿。 */
    return tool_fail("MCP server 不在手上: " + server);
}

namespace {
/* 权限策略：一个配置键，一个 switch（ADR-0016）。
 *   ask       —— 危险工具一律问用户（默认）
 *   allow-all —— 一律放行。打通链路用，不是安全策略
 *   deny      —— 一律拒绝
 * 认不出的值按 ask：配置写错时该多问一句，不该多放一次行。 */
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
    if (!tool_dangerous(tool)) return true; // 只读工具不触发
    switch (decide(*ctx_.config))
    {
        case Verdict::Allow:
            return true;
        case Verdict::Deny:
            if (denied_reason) *denied_reason = "denied by permission policy";
            return false;
        case Verdict::Ask:
            // 没有客户端连着就当场拒绝，不等那 30 秒（ADR-0019 §8）：agent 没有客户端
            // 也照跑，两条合起来就是后台 agent 的每个危险工具都卡 30 秒然后必然被拒——
            // 那不是安全策略，是一个装成策略的超时。当场拒是同一个结论，早 30 秒给出，
            // 而且能说一句诚实的话
            if (!approval_.online())
            {
                if (denied_reason) *denied_reason = "无客户端可裁决";
                return false;
            }
            // 审批链路（ADR-0005）：core 发 permission_request → 用户裁决 → 回传。
            // agent 线程真等裁决（30s 超时 deny），事件循环线程收 /approval-response 唤醒
            if (approval_.await(agent_id_, tool.value("name", std::string()), params_json) != Verdict::Allow)
            {
                if (denied_reason) *denied_reason = "denied by user";
                return false;
            }
            return true;
    }
    return false;
}

void Executor::interrupt()
{
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = true;
    if (inflight_) interrupt_tool(); // 手上没有在跑的：标记留给下一次 execute 撞上
}

void Executor::reset()
{
    std::lock_guard<std::mutex> lk(inflight_mtx_);
    interrupted_ = false;
}

nlohmann::json Executor::execute(const std::string &call_id, const std::string &name,
                                 const std::string &params_json)
{
    /* 结果的三个字段（content / isError / interrupted）由每条返回路径写齐——
     * 少写一个，调用方就得给它准备一个默认值，那个默认值又会掩盖是谁漏写的。 */
    const auto bail = [](nlohmann::json r, bool intr) {
        r["interrupted"] = intr;
        return r;
    };

    const nlohmann::json *tool = find(name);
    if (!tool) return bail(tool_fail("unknown tool"), false);

    std::string reason;
    if (!check_permission(*tool, params_json, &reason)) return bail(tool_fail(reason), false);

    /* preHook：**只能收紧，不能放宽**（ADR-0024 §6）。它排在 permission 之后——
     * 一个 hook 永远不能把 ask 变成 allow，因为它根本走不到还没裁决的那一步。 */
    if (hooks_ && !hooks_->empty())
    {
        nlohmann::json p{{"matcher_key", name}, {"agent_id", agent_id_}, {"cwd", workdir_}, {"tool_name", name}, {"tool_input", params_json}};
        if (const HookOutcome h = hooks_->run(HookEvent::PreToolUse, p, &interrupted_); h.deny)
            return bail(tool_fail(h.reason.empty() ? "被 hook 拦下" : h.reason), false);
    }

    // 登记在先、执行在后：这个顺序才让 interrupt() 要么打断得到它、要么撞上 interrupted_，
    // 不存在"检查完了才开始跑"的缝
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        if (interrupted_) return bail(tool_fail("interrupted by user"), true);
        inflight_ = true;
    }
    /* 参数**只解析一次**，解出来的往下传。模型给的形状不由 core 说了算，解不动就当空对象。 */
    nlohmann::json params = nlohmann::json::parse(params_json, nullptr, false);
    if (params.is_discarded()) params = nlohmann::json::object();
    /* 唯一那个分支：**代码在 core 里，还是在别的进程里**（ADR-0023 §1）。
     * spawn / send_message 那两个是本来就在的账（定义在 tools/，实现在这儿），跟 MCP 无关。 */
    nlohmann::json r;
    if (!tool_server(*tool).empty())
        r = mcp_call(*tool, params);
    else if (name == "spawn" || name == "send_message")
        r = agent_tool(name, params);
    else
        r = run_tool(call_id, name, params, ctx_.emit_fn, workdir_);
    {
        std::lock_guard<std::mutex> lk(inflight_mtx_);
        // "算不算被中断"由 core 判——中止是 core 提的，工具不必编造状态码
        r["interrupted"] = interrupted_.load();
        inflight_ = false;
    }

    /* postHook。**判据是「有没有产出一个结果」**：工具报错算做完了，post 要跑并拿得到
     * 那个结果；被中断的那件事没做完，post 不跑（ADR-0024 §6）。不用 RAII——
     * 它在这两种情况下的行为恰好是反的。 */
    if (hooks_ && !hooks_->empty() && !r.value("interrupted", false))
    {
        nlohmann::json p{{"matcher_key", name},
                         {"agent_id", agent_id_},
                         {"cwd", workdir_},
                         {"tool_name", name},
                         {"tool_input", params_json},
                         {"tool_response", r.value("content", nlohmann::json::array())}};
        hooks_->run(HookEvent::PostToolUse, p, &interrupted_);
    }
    return r;
}

/* 取一个整数数组参数；缺失/形状不对当空。参数是模型给的，形状不由 core 说了算。 */
static std::vector<int> id_list(const nlohmann::json &p, std::string_view key)
{
    std::vector<int> out;
    const auto it = p.find(key);
    if (it == p.end() || !it->is_array()) return out;
    for (const auto &v : *it)
    {
        if (v.is_number_integer() || v.is_number_unsigned())
            out.push_back(v.get<int>());
    }
    return out;
}

nlohmann::json Executor::agent_tool(const std::string &name, const nlohmann::json &params)
{
    if (!pool_) return tool_fail("no agent graph here");

    const auto str = [&params](std::string_view k) {
        const auto it = params.find(k);
        return it != params.end() && it->is_string() ? it->get<std::string>() : std::string();
    };

    if (name == "send_message")
    {
        const auto it_to = params.find("to");
        if (it_to == params.end() || (!it_to->is_number_integer() && !it_to->is_number_unsigned()))
            return tool_fail("send_message is missing or has an invalid target agent id: to");
        const int to = it_to->get<int>();
        if (!pool_->post(to, str("text")))
            return tool_fail("no such agent: " + std::to_string(to));
        return tool_ok("sent to " + std::to_string(to));
    }

    /* spawn：派生方决定新 agent 的全部出入边（ADR-0019），也由它解析 `agent`（ADR-0024 §8）——
     * 它在自己的 system prompt 里看见了哪些名字，就该拿到哪一份正文。让被派生方按自己的
     * workdir 再查一次，会出现「模型看见的名字在那边不存在」。
     * 查的就是写进 system prompt 的那张表（`defs_`），不是回头问 pool 要。 */
    std::string def_body;
    if (const std::string want = str("agent"); !want.empty())
    {
        const AgentDef *d = nullptr;
        if (defs_)
            for (const AgentDef &x : *defs_)
                if (x.name == want)
                {
                    d = &x;
                    break;
                }
        // 认不出就当场说，不悄悄派生一个没接上正文的 agent
        if (!d) return tool_fail("unknown agent definition: " + want);
        def_body = d->body;
    }

    std::string err;
    const int id = pool_->create(str("workdir"), agent_id_, id_list(params, "in_edges"),
                                 id_list(params, "out_edges"), err, def_body);
    if (id <= 0) return tool_fail(err);
    pool_->post(id, str("prompt"));
    return tool_ok(std::to_string(id));
}

} // namespace realagent
