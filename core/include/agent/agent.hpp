/*
 * agent.hpp — agent loop（M3 核心）
 *
 * 形态（ADR-0002/ADR-0007）：while(1)——
 *   调 LLM（造请求 → curl 发出 → 解析 SSE → 计价）→ 广播事件
 *   → 有 tool_use 则顺序执行工具、结果入会话、继续循环；否则完成
 *
 * core 持有抽象对话（JSON），llm 模块负责转成 /v1/messages 的具体形状。
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

#include "agent/agent_defs.hpp"
#include "agent/commands.hpp"
#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "agent/hooks.hpp"
#include "agent/session.hpp"
#include "agent/skills.hpp"
#include "json.hpp"
#include "llm/llm.hpp"
#include "mcp/mcp.hpp"

namespace realagent {

class Agents;

/* 一次 LLM 调用的产出 */
struct LlmOutcome {
    std::string text;               // 累积文本（非 tool_use 时）
    std::string thinking;           // 思考内容（流式累积）
    std::string thinking_signature; // thinking 块签名（回传历史用）
    struct ToolUse {
        std::string id;
        std::string name;
        std::string input; // JSON 对象字符串
    };
    std::vector<ToolUse> tool_uses;
    std::string stop_reason;
    std::string error; // 非空 = 本次调用失败的人话原因（广播给客户端）
    double cost = 0;   // 本次调用花掉的钱（USD，绝对值，后到覆盖先到）
};

class Agent {
  public:
    /* workdir 是 agent 干活的地方，**必传、无默认、不从 cwd 取**（ADR-0019）：
     * 会话落在 <workdir>/.realagent/sessions，工具的相对路径也从这里算起。 */
    /* sub = 这个 agent 是别的 agent 派生出来的。会话落点因此不同（ADR-0021）：
     * 客户端建的落 `<workdir>/.realagent/sessions/`，派生的落 `.../sessions/sub/`。
     * **两边都落盘**——不落盘的那份内存里丢不掉（idle 也释放不了），而且出了事查不了。
     * 清单只扫顶层，于是「不在会话列表里显示」不是一个开关，是落点的后果。 */
    /* def_body = agent 定义的正文（ADR-0024 §8），接在 system prompt 尾部。
     * **core 那段永远在前**——只有一个 `system_prompt()`，派生出来的走同一条路，
     * 拿不到「不带 stop 契约」的 system prompt。 */
    Agent(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir, int id,
          Agents *pool = nullptr, bool sub = false, std::string def_body = {});

    int id() const { return id_; }
    /* 这个 agent 看得见的 prompt 命令（GET /commands 与派发共用这一张表）。 */
    const std::vector<PromptCommand> &commands() const { return commands_; }

    /* 建这个 agent 时 plugin 那边出的问题：MCP 没连上的、hooks.json 读坏的。
     * 它们本来只进 stderr，而 core 是常驻服务、用户看不见——`/plugins` 是唯一
     * 把这笔账还给用户的地方（ADR-0023 §8）。 */
    std::vector<std::string> plugin_errors() const
    {
        std::vector<std::string> all = mcp_.errors;
        all.insert(all.end(), hooks_.errors().begin(), hooks_.errors().end());
        return all;
    }

    /* 跑一遍 SessionStart hook，把它注入的文字记下来（ADR-0024 §6）。
     * source 就是 Claude Code 那几个词（`startup` / `clear` / `resume`），
     * hook 的 matcher 拿它来挑——名字照抄，改一个字母就等于吃不下现成的 hooks.json。
     *
     * 建 agent 时一次，`/new` 与 `/resume` 之后各一次：那三处都是"会话换了一份"。
     * 注入的文字进 system prompt，与 skill 清单同一个位置。 */
    void session_start_hook(const std::string &source);

    const std::string &workdir() const { return workdir_; }

    /* 这个 agent 的 system prompt。**只有这一处拼**（`build_dialog` 也是问它要的）：
     * core 那段与 `stop` 契约永远在前，skill 清单、agent 定义清单、派生方交下来的
     * 那份正文、SessionStart 注入的那段依次接在后面。 */
    std::string system_prompt() const;

    /* 会话目录（`<workdir>/.realagent/sessions`）。GET /sessions 扫的就是它——
     * 会话跟着 agent 的工作目录走，不是进程级的（ADR-0019）。 */
    const std::string &session_dir() const { return session_dir_; }

    ~Agent();

    /* 投一条消息进收件箱。三种来源一个队列（ADR-0019）：人发的、别的 agent 发的、
     * 别的 agent 跑完的通知。取走时不分辨它是哪来的。
     * 正在跑也照投不误：下一个 turn 开头就会被一并取走（ADR-0019 §5）。 */
    void post(std::string message);

    /* 正在推进一个 Turn（不是 idle）。GET /agents 报状态用。 */
    bool running() const { return running_.load(); }

    /* 事件循环线程要动这个 agent 的历史（/new、/resume、列会话）时先拿这把锁。
     * 拿不到就回一句"忙着呢"，绝不排队等——等的是一次 run 跑完，期间连
     * POST /interrupt 都收不了（ADR-0017）。 */
    std::unique_lock<std::mutex> try_lock() { return {run_mtx_, std::try_to_lock}; }

    /* 中断当前 run（任意线程安全；curl/工具/turn 间均检查） */
    void interrupt();

    /* 内存里此刻留着几条历史。**idle 时是 0**（ADR-0019 §7）。
     * 要问「这个 agent 记了什么」问盘（`Session::read`），不问这里——
     * 这个数只回答「内存里现在占着多少」。 */
    size_t resident() const { return messages_.size(); }

    /* 新建会话（/new 命令）：清空历史 + 换一个会话文件。
     * 旧文件不动——它是记录，不是缓存。 */
    void reset();

    /* 恢复会话（/resume <id>）：JSONL 读回历史，后续追加写进该文件。
     * 读不到返回 false，此时当前会话**原样不动**（宁可恢复失败，也不能把人扔进空白）。*/
    bool resume(const std::string &id);

    /* 当前会话 id（GET /sessions 标出 current，客户端据此知道自己在哪儿） */
    const std::string &session_id() const { return session_.id(); }

    /* SseParser 产出的事件落在这里：累积进 out，该实时广播的顺手广播。
     * public 只因为 libcurl 的写回调是个自由函数，进不来私有区。 */
    void on_llm_event(std::string_view type, const nlohmann::json &ev, LlmOutcome &out,
                      const std::string &model);

  private:
    /* agent 主循环，整条线程从头到尾就在这里面。**一层循环，一圈一个 turn**：
     * 等到有活干（上一圈没收工，或收件箱非空）→ 把攒着的消息全收进来 → 调 LLM →
     * 执行工具 → 模型调了 `stop` 才收工，回去等。
     * 两样都没有就阻塞——那就是 idle（ADR-0019），不需要「空闲」这个状态。 */
    void loop();

    /* idle ⇄ 运行中那条边沿。「一趟」不是第二层循环，就是这两个函数之间那段；
     * busy（run_mtx_）的持有与否即「在不在一趟中间」，不另存状态位。 */
    void start_run(std::unique_lock<std::mutex> &busy);
    void finish_run(std::unique_lock<std::mutex> &busy);

    /* 收件箱里此刻攒着的全部入账（可能一条也没有） */
    void take_inbox();
    /* 一条从外面来的消息入账 + 广播。人发的、别的 agent 发的、完成通知、core 递的话头，
     * 全走这里——凡是从 agent 外面来的输入都是 user（ADR-0019 §5）。 */
    void record_user(const std::string &text);
    /* 一次 LLM 产出入账（thinking + 正文 + tool_use，块序由协议定） */
    void record_assistant(const LlmOutcome &out);
    /* 顺序执行这一批工具，结果逐条入账。返回：模型打了 `stop` 没有。
     * 中断不由返回值表达——abort_ 调用方自己看得见。 */
    bool run_tools(const LlmOutcome &out);

    /* idle 时历史还给了盘，醒来先读回来。读不到 = 这个会话还没写过盘，
     * 空历史就是对的，不是错。 */
    void ensure_loaded();

    /* 消息入账的唯一入口：进内存，同时落盘。
     * run() 里有六处产生消息，全都走这里——散着写 push_back 迟早漏掉一处，
     * 而漏掉的那条恢复会话时就凭空消失了。 */
    void record(nlohmann::json msg);

    /* 构建抽象对话（system/messages/tools），tier 决定 dialog["model"] 取哪一档 */
    nlohmann::json build_dialog(ModelTier tier) const;
    /* 一次 LLM 调用：build_request → libcurl → SseParser → LlmOutcome */
    bool llm_call(const nlohmann::json &dialog, LlmOutcome &out);
    /* 最后一条 assistant 消息的正文（完成通知带的就是它） */
    std::string last_text() const;
    /* 广播事件（走 CoreContext::emit_fn） */
    void broadcast(const std::string &type, const nlohmann::json &payload);

    CoreContext &ctx_;
    Agents *pool_ = nullptr; // 跑完时沿入边通知邻居；独立构造（测试）时为空
    int id_ = 0;
    std::string workdir_;
    /* 这个 agent 看得见的 MCP 工具与连接（ADR-0023）。**建 agent 时开一次**，
     * 与 workdir、skill 清单同期确定、同期不变——一趟中间清单变形，prompt cache 当场碎。
     * 连接本身在进程级的池子里按配置去重，这里拿的是 shared_ptr：最后一个 agent 松手，
     * server 进程才收尾。**排在 exe_ 前面**：Executor 拿的是指向它的指针。
     *
     * ADR-0023 说的是「生死跟着组」，而组（ADR-0021）还没建——今天持有者是 agent。
     * 换成组只是换谁拿这个 Lease，计数机制不变（池里存的是 weak_ptr）。 */
    McpHub::Lease mcp_;
    /* 这个 agent 看得见的 hook（ADR-0024 §6）。**排在 exe_ 前面**：Executor 拿的是
     * 指向它的指针。同 skill 与命令，创建时扫一次、同期不变。 */
    Hooks hooks_;
    /* 这个 agent 看得见的 agent 定义（ADR-0024 §8）：它派生别人时用得上，所以进自己的
     * system prompt，`spawn` 的 `agent` 参数也认这张表。**排在 exe_ 前面**：
     * Executor 拿的是指向它的指针（同 mcp_ 与 hooks_）。 */
    std::vector<AgentDef> agent_defs_;
    /* Executor 的 inflight_ / interrupted_ 是**这一个 agent** 的状态，
     * 共享一个就会串味：中断 A 会把 B 的下一次工具调用一起拒掉。 */
    Executor exe_;
    /* 对话历史。**idle 时它是空的**：那份内存是盘上那份的逐字副本
     * （`Session::append` 每条消息即时追加），丢掉不丢信息，醒来重读一遍
     * （ADR-0019 §7）。线程是 16KB，这个是 MB 级——几十个 idle agent 就是几百 MB，
     * 全在等一条可能永远不来的消息。
     * 要问「这个 agent 记了什么」，问盘（`Session::read`），不问这里。 */
    nlohmann::json messages_;
    bool loaded_ = true;      // 新 agent 盘上本来就没有，空历史就是对的
    std::string session_dir_; // <workdir>/.realagent/sessions
    Session session_;         // 当前会话的落盘去处（JSONL，append-only）
    /* 这个 agent 看得见的 skill（ADR-0022）。**创建时扫一次**，与 workdir 同期确定、
     * 同期不变：改了 skill 开个新 agent，跟配置「启动读一次」（ADR-0010）同一个作风。 */
    std::vector<Skill> skills_;
    /* 这个 agent 看得见的 prompt 命令（ADR-0024 §7）。同上，创建时扫一次。
     * 它不进 system prompt——命令是用户显式触发的，模型不需要知道有哪些。 */
    std::vector<PromptCommand> commands_;
    /* SessionStart hook 注入的那段文字，拼在 system prompt 尾部。**一个 hook 都没有
     * 时它是空串**——system prompt 与加这个功能之前一个字不差（同 skills_prompt）。 */
    std::string session_context_;
    /* 派生它的那个 agent 交下来的那份 agent 定义正文。空 = 没指定。 */
    std::string def_body_;
    double run_cost_ = 0; // 本次 run 累计花费（USD），一次用户输入起算清零
    std::atomic<bool> abort_{false};

    // 收件箱与跑它的那条线程。idle 就是阻塞在 cv_ 上——不占 CPU，也不需要"空闲"这个状态
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::string> inbox_;
    bool closing_ = false;
    std::atomic<bool> running_{false};
    std::mutex run_mtx_; // 跑一条消息期间持有，见 try_lock()
    std::thread loop_;
};

} // namespace realagent
