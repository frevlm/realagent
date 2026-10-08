/*
 * agent.hpp — 一个 agent：一条线程、一个收件箱、一份会话
 *
 * 循环一圈一个 turn：收件箱入账 → 调主模型 → 执行工具。主模型一个工具都没调时，
 * 由小模型判这一趟干完没有（ADR-0025）；没干完就打回接着跑。
 * idle 时阻塞在收件箱上，历史还给盘（ADR-0019 §7）。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agent/catalog.hpp"
#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "agent/hooks.hpp"
#include "agent/session.hpp"
#include "agent/verdict.hpp"
#include "json.hpp"
#include "llm/llm.hpp"
#include "mcp/mcp.hpp"

namespace realagent {

class Agents;

/* 一次 LLM 调用的产出 */
struct LlmOutcome {
    std::string text;
    std::string thinking;
    std::string thinking_signature;
    struct ToolUse {
        std::string id;
        std::string name;
        std::string input; // JSON 对象文本
    };
    std::vector<ToolUse> tool_uses;
    std::string stop_reason;
    std::string error; // 非空 = 失败原因（给人看）
    double cost = 0;   // USD
};

class Agent {
  public:
    /* ctx 拷一份留着：事件出口与「有没有人能裁决」是这个 agent 那一组的。
     * workdir 必传，工具的相对路径从它算起。
     * session_dir 为空就落 sessions_dir(workdir)；派生的由 Agents 指到对话那一头的 sub/。
     * root = 用户看见的那段对话的 session_id，为空就是自己（ADR-0029）。
     * def_body = agent 定义正文，接在 system prompt 末尾。
     * session_id 为空就开一个新会话；盘上有就接着它往下说。 */
    Agent(CoreContext ctx, ApprovalCoordinator &approval, std::string workdir, int id,
          Agents *pool = nullptr, std::string session_dir = {}, std::string root = {},
          std::string def_body = {}, std::string session_id = {});
    ~Agent();

    int id() const { return id_; }
    const std::string &workdir() const { return workdir_; }
    const std::string &session_dir() const { return session_dir_; }
    const std::string &session_id() const { return session_.id(); }
    const std::string &root() const { return root_; }

    /* 建 agent 时 MCP 连不上、hooks.json 读坏的那些，给 /plugins 看。 */
    std::vector<std::string> plugin_errors() const;

    /* core 那段、skill 清单、agent 定义清单、def_body、SessionStart 注入依次拼接。 */
    std::string system_prompt() const;

    /* 跑 SessionStart hook，注入的文字进 system prompt。
     * source 是 Claude Code 的词：startup / clear / resume。 */
    void session_start_hook(const std::string &source);

    /* 投一条消息进收件箱（人、别的 agent、完成通知都走这里）。正在跑也照投，
     * 下一个 turn 开头取走。notice = 这是一条完成通知。 */
    void post(std::string message, bool notice = false);

    bool running() const { return running_.load(); }

    /* 任意线程。停住 LLM 流与在跑的工具。 */
    void interrupt();

    /* 内存里留着几条历史；idle 时是 0。 */
    size_t resident() const { return messages_.size(); }

    /* SseParser 产出的事件落到这里（public 只因 curl 回调是自由函数）。
     * silent：正文不推给客户端（收工判定那次），花费照报。 */
    void on_llm_event(std::string_view type, const nlohmann::json &ev, LlmOutcome &out,
                      const std::string &model, bool silent);

  private:
    void loop();
    StopVerdict judge();

    /* 一趟的两条边沿：开跑上 run_mtx_，收工解锁并通知邻居。 */
    void start_run(std::unique_lock<std::mutex> &busy);
    void finish_run(std::unique_lock<std::mutex> &busy);

    void take_inbox();
    void record_user(const std::string &text);
    void record_assistant(const LlmOutcome &out);
    void run_tools(const LlmOutcome &out);
    /* 消息入账的唯一入口：先落盘再进内存。 */
    void record(nlohmann::json msg);
    void ensure_loaded();

    nlohmann::json build_dialog(ModelTier tier) const;
    bool llm_call(const nlohmann::json &dialog, LlmOutcome &out, bool silent = false);
    std::string last_text() const;
    void broadcast(const std::string &type, const nlohmann::json &payload);

    CoreContext ctx_;        // 排第一：exe_ 拿的是它的引用
    Agents *pool_ = nullptr; // 测试里独立构造时为空
    int id_ = 0;
    std::string workdir_;
    /* 以下几样建 agent 时扫一次，之后不变。mcp_ / hooks_ / agent_defs_ 要排在 exe_ 前面：
     * Executor 拿的是指向它们的指针。 */
    McpHub::Lease mcp_;
    Hooks hooks_;
    std::vector<AgentDef> agent_defs_;
    Executor exe_;
    std::vector<Skill> skills_;

    std::string session_dir_;
    Session session_;
    std::string root_;
    nlohmann::json messages_ = nlohmann::json::array(); // idle 时清空，醒来从盘上读回
    bool loaded_;
    std::string session_context_; // SessionStart hook 注入的文字
    std::string def_body_;

    size_t run_begin_ = 0; // 这一趟开跑时历史的长度：收工判定的抄本从这里取
    std::string recap_;    // 裁判写的 recap；没有裁判时为空
    int stall_ = 0;        // 连着几次打回后主模型仍一个工具都没调
    double run_cost_ = 0;  // 这一趟累计花费
    std::atomic<bool> abort_{false};

    std::mutex mtx_; // 护 inbox_ / closing_
    std::condition_variable cv_;
    struct Mail {
        std::string text;
        bool notice; // 完成通知：只是消息，不是请求
    };
    std::deque<Mail> inbox_;
    bool asked_ = false; // 这一趟收到过不是完成通知的东西
    bool closing_ = false;
    std::atomic<bool> running_{false};
    std::mutex run_mtx_; // 一趟期间持有
    std::thread loop_;
};

} // namespace realagent
