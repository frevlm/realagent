#include "agent/agent.hpp"

#include "agent/agents.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace realagent {

namespace {
/* 会话落点：客户端建的落 sessions/，派生的落 sessions/sub/（ADR-0021）。
 * 不用拼一个空段——`fs::path / ""` 会留一个尾斜杠。 */
std::string session_dir_of(const std::string &workdir, bool sub)
{
    const std::filesystem::path base = std::filesystem::path(workdir) / ".realagent" / "sessions";
    return (sub ? base / "sub" : base).string();
}
} // namespace

Agent::Agent(CoreContext &ctx, ApprovalCoordinator &approval, std::string workdir, int id,
             Agents *pool, bool sub, std::string def_body)
    : ctx_(ctx), pool_(pool), id_(id), workdir_(std::move(workdir)),
      mcp_(ctx.mcp ? ctx.mcp->open(workdir_, ctx.config->get_json("mcp_http_bridge"))
                   : McpHub::Lease{}),
      hooks_(Hooks::scan(workdir_)), agent_defs_(scan_agent_defs(workdir_)),
      exe_(ctx, approval, workdir_, pool_, id_, &mcp_, &hooks_, &agent_defs_),
      session_dir_(session_dir_of(workdir_, sub)),
      session_(session_dir_), skills_(scan_skills(workdir_)), commands_(scan_commands(workdir_)),
      def_body_(std::move(def_body))
{
    /* 连不上的 server 只报不拦（models.json 那条先例）。用户当下看不见这行——
     * core 是常驻服务，面前只有 TUI。这笔债 skill 也欠着，一并另算（ADR-0023 §8）。 */
    for (const std::string &e : mcp_.errors) fprintf(stderr, "[mcp] %s\n", e.c_str());
    for (const std::string &e : hooks_.errors()) fprintf(stderr, "[hook] %s\n", e.c_str());
    if (!mcp_.tools.empty())
        fprintf(stderr, "[mcp] agent %d: %zu servers, %zu tools\n", id_, mcp_.conns.size(),
                mcp_.tools.size());

    messages_ = nlohmann::json::array();
    /* SessionStart：建 agent 就是一次「会话换了一份」。**这会阻塞建 agent**——
     * 每个匹配的 hook 一个进程，各自最多 timeout 那么久。装即授权，也就装即承担。 */
    session_start_hook("startup");
    loop_ = std::thread([this] { loop(); });
}

Agent::~Agent()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        closing_ = true;
    }
    cv_.notify_all();
    interrupt(); // 正卡在 LLM 或工具里的话，先把它停下来，别等它自己跑完
    if (loop_.joinable()) loop_.join();
}

void Agent::post(std::string message)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        inbox_.push_back(std::move(message));
    }
    cv_.notify_one();
}

void Agent::ensure_loaded()
{
    if (loaded_) return;
    if (!Session::read(session_dir_, session_.id(), messages_))
        messages_ = nlohmann::json::array(); // 还没写过盘，不是错
    loaded_ = true;
}

void Agent::reset()
{
    messages_ = nlohmann::json::array();
    loaded_ = true;
    session_ = Session(session_dir_); // 换个新会话文件；旧的留在盘上，它是记录不是缓存
    run_cost_ = 0;
    abort_.store(false);
}

bool Agent::resume(const std::string &id)
{
    nlohmann::json loaded;
    Session s(session_dir_);
    if (!s.resume(id, loaded)) return false; // 先在副本上试，成了才动自己
    session_ = std::move(s);
    messages_ = std::move(loaded);
    loaded_ = true;
    run_cost_ = 0;
    abort_.store(false);
    return true;
}

void Agent::record(nlohmann::json msg)
{
    session_.append(msg); // 先落盘：进了内存却没进文件，恢复时就是一条凭空消失的消息
    messages_.push_back(std::move(msg));
}

void Agent::interrupt()
{
    abort_.store(true);
    // 统一信号（PLAN.md R8）：一个动作同时停住 LLM 流与在跑的工具。
    // 从事件循环线程进来——agent 线程此刻多半正卡在某个工具里，指望不上它自己检查
    exe_.interrupt();
}

/* 最后一条 assistant 消息的正文。完成通知带的就是它——那是这次跑出来的产物。 */
std::string Agent::last_text() const
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
    {
        if (it->value("role", "") != "assistant") continue;
        std::string out;
        for (const auto &b : (*it)["content"])
            if (b.value("type", "") == "text") out += b.value("text", "");
        return out;
    }
    return {};
}

void Agent::broadcast(const std::string &type, const nlohmann::json &payload)
{
    if (!ctx_.emit_fn) return;
    // 每帧带 agent_id：core **不为任何 agent 过滤事件**，全推，客户端认识哪个渲染哪个
    // （ADR-0019）。于是杂活 agent 失败时用户看得见——这不需要为它设计任何东西，
    // 只需要不设计过滤。
    nlohmann::json ev = payload.is_object() ? payload : nlohmann::json::object();
    ev["agent_id"] = id_;
    ctx_.emit_fn(type, ev.dump());
}

/* 一次 LLM 调用期间的流式状态：解析器 + 落点。
 * 解析器按调用建、按调用扔——上一轮的半截 SSE 缓冲绝不该漏进下一轮。 */
struct StreamCtx {
    Agent *self;
    CURL *curl = nullptr; // 写回调里问状态码要用（头已经收完了，问得到）
    SseParser parser;     // 协议决定怎么解，一次调用一个实例
    LlmOutcome *out;
    std::string model;         // 本次调用的模型名（计价按它查单价）
    bool parse_failed = false; // 解析报错：与用户中断区分开
    long status = 0;           // 首次拿到响应体时问一次 HTTP 状态码
    std::string error_body;    // 状态码不是 2xx 时，响应体不是流，攒起来给人看
};

static int curl_progress_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    auto *abort = static_cast<const std::atomic<bool> *>(clientp);
    return (abort && abort->load()) ? 1 : 0;
}

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *s = static_cast<StreamCtx *>(userdata);
    const size_t n = size * nmemb;

    /* 先看状态码，再谈解析（ADR-0017）。curl 对 401/500 一律返回 CURLE_OK——
     * HTTP 层的失败不是传输层的失败。错误体不是 SSE：喂给解析器切不出事件块，
     * 最后攒出一个"成功但空"的回答，用户看不出发生了什么，会话里还留下一条空消息。 */
    if (s->status == 0) curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &s->status);
    if (s->status >= 400)
    {
        // 攒错误体给人看，别喂解析器。攒够 8KB 就够说明问题了
        if (s->error_body.size() < 8192) s->error_body.append(ptr, n);
        return n;
    }

    const bool ok = s->parser.feed(std::string_view(ptr, n), [s](std::string_view t, const nlohmann::json &ev) {
        s->self->on_llm_event(t, ev, *s->out, s->model);
    });
    // 解析失败：立刻中止传输。继续读下去只会攒出一个"成功但空"的回答，
    // 那比报错更糟——用户看不出发生了什么。（返回 < n 即令 curl 报 CURLE_WRITE_ERROR）
    if (!ok)
    {
        s->parse_failed = true;
        return 0;
    }
    return n;
}

void Agent::on_llm_event(std::string_view type, const nlohmann::json &ev, LlmOutcome &out,
                         const std::string &model)
{
    if (type == "message_update")
    {
        out.text += ev["delta"].get<std::string>();
        broadcast("message_update", ev); // 实时增量 → TUI 打字效果
    }
    else if (type == "thinking_start")
    {
        out.thinking_signature = ev["signature"];
        broadcast("thinking_start", ev);
    }
    else if (type == "thinking_update")
    {
        out.thinking += ev["delta"].get<std::string>();
        broadcast("thinking_update", ev);
    }
    else if (type == "thinking_stop")
    {
        broadcast("thinking_stop", ev);
    }
    else if (type == "usage")
    {
        // 计价（ADR-0009）：token 用量换成钱，**usage 事件本身不再上传**——
        // 客户端不认识 token。算不出（表里没这个模型）就什么都不发，不发 0。
        //
        // 本次用的是哪个模型，调用方自己知道（就是 dialog["model"]）——
        // 从前这条信息要靠 provider 壳在改请求时偷偷记一笔，现在直接传进来
        const double cost = ctx_.pricing ? ctx_.pricing->cost(model, ev) : 0;
        if (cost <= 0) return;
        out.cost = cost;
        // 推送流里的花费一律是"本次 run 累计"
        broadcast("status_update", nlohmann::json{{"cost", run_cost_ + cost}});
    }
    else if (type == "tool_use")
    {
        LlmOutcome::ToolUse tu;
        tu.id = ev["id"];
        tu.name = ev["name"];
        tu.input = ev["input"].is_null() ? "{}" : ev["input"].dump();
        out.tool_uses.push_back(std::move(tu));
    }
    else if (type == "stop")
    {
        out.stop_reason = ev["reason"];
    }
}

bool Agent::llm_call(const nlohmann::json &dialog, LlmOutcome &out)
{
    /* 端点那一束没配齐就别往下走（ADR-0017）。
     *
     * main 在 POST /message 上挡过一道，但那只是**一个**门：收件箱之后消息还会从
     * 别的门进来（另一个 agent 的 send_message、完成通知）。而 build_request 里
     * 解析协议是个断言——解不出来直接 throw，那会 terminate 整个常驻服务。
     * core 是常驻的，一条消息不该杀掉它；把该配什么原样交给用户才是 ADR-0017 要的。 */
    if (out.error = endpoint_config_error(*ctx_.config); !out.error.empty())
    {
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }

    const HttpRequest req = build_request(*ctx_.config, dialog);
    if (req.url.rfind("http", 0) != 0)
    {
        // base_url 配了但拼出来不是个 URL。libcurl 会报一句难懂的错，不如在这儿说人话
        out.error = "base_url 不像个 URL（当前请求 URL: " + req.url + "）";
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) return false;
    // 协议在这一层就定下来：解析器建出来那一刻就知道自己要解哪套帧
    StreamCtx s{.self = this,
                .curl = curl,
                .parser = SseParser(*protocol_from(ctx_.config->get("protocol"))),
                .out = &out,
                .model = dialog.value("model", std::string())};
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_);
    struct curl_slist *hdrs = nullptr;
    for (const auto &h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    const CURLcode rc = curl_easy_perform(curl);
    s.parser.flush([](std::string_view, const nlohmann::json &) {});
    curl_easy_cleanup(curl);
    if (hdrs) curl_slist_free_all(hdrs);

    // HTTP 状态码优先报：它是最上游的真因。401 的错误体不是流，
    // 解析器切不出事件也算"没解析失败"——只看 parse_failed 会把认证失败说成成功
    if (const std::string he = http_status_error(s.status, s.error_body); !he.empty())
    {
        out.error = he;
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }
    // 解析失败次之：curl 的 CURLE_WRITE_ERROR 只是我们主动中止的副作用，不是真因
    if (s.parse_failed)
    {
        out.error = "解析响应失败（见 core 日志）";
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }
    if (rc != CURLE_OK)
    {
        out.error = std::string("curl 失败: ") + curl_easy_strerror(rc);
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    }
    return true;
}

std::string Agent::system_prompt() const
{
    // stop 的契约必须写在这儿。模型不知道有这个出口就不会调它
    return "You are a helpful coding agent. Your agent id is " + std::to_string(id_) +
           ". Your working directory is " + workdir_ + ".\n"
                                                       "You run in an autonomous loop: calling `stop` is the only way to end your run. Whenever you finish answering a question or completing a task, output your response and call `stop` in the same turn. Do not invent tasks the user did not request.\n\n"
                                                       "Example:\n"
                                                       "User: Explain this function.\n"
                                                       "Assistant: [Explains the function] + tool_call: stop()" +
           // skill 清单（ADR-0022）。一个都没有时这里是空串——system prompt 与加这个功能之前一个字不差
           skills_prompt(skills_) +
           // 可以拿去 spawn 的 agent 定义（ADR-0024 §8）。同上，一个都没有就是空串
           agent_defs_prompt(agent_defs_) +
           // 派生方交下来的那份 agent 定义正文。**接在最后**：core 那段与 stop 契约永远在它前面
           (def_body_.empty() ? std::string() : "\n\n" + def_body_) +
           // SessionStart hook 注入的那段（ADR-0024 §6）。同上，没有就是空串
           (session_context_.empty() ? std::string() : "\n\n" + session_context_);
}

nlohmann::json Agent::build_dialog(ModelTier tier) const
{
    nlohmann::json dialog;
    dialog["model"] = ctx_.config->model(tier);
    dialog["system"] = system_prompt();
    /* 工具定义**就是端点要的那个对象**：拷一份，抹掉 core 私有的那一个键（ADR-0023 §2）。
     * LLM 见到的名字与 executor 查表用的名字是同一个。 */
    nlohmann::json tools = tool_defs();
    for (const nlohmann::json &t : mcp_.tools) tools.push_back(t); // MCP 来的接在后面
    for (auto &t : tools) t.erase("_core");
    dialog["tools"] = std::move(tools);
    dialog["messages"] = messages_;
    return dialog;
}

void Agent::session_start_hook(const std::string &source)
{
    session_context_.clear();
    if (hooks_.empty()) return;
    nlohmann::json p{{"matcher_key", source},
                     {"agent_id", id_},
                     {"cwd", workdir_},
                     {"source", source},
                     {"session_id", session_.id()}};
    // deny 在这个位置拦不住任何东西（会话已经换了），hooks.cpp 会为此报一条
    session_context_ = hooks_.run(HookEvent::SessionStart, p, &abort_).inject;
}

/* 模型回了话却没调工具。提醒它完成即 stop，同时防止其臆想新任务乱执行 */
static constexpr const char *kStopReminder =
    "You replied without calling any tools. If you have finished the user's request, call `stop` now. Do not invent new tasks.";

/* —— idle ⇄ 运行中那条边沿。busy 就是「在不在一趟中间」，在这两处上锁、解锁 —— */

void Agent::start_run(std::unique_lock<std::mutex> &busy)
{
    busy.lock(); // 见 try_lock()：事件循环线程拿不到就回一句"忙着呢"，不排队等
    running_.store(true);
    ensure_loaded(); // idle 期间历史还给了盘，先读回来
    abort_.store(false);
    exe_.reset(); // 中止痕迹与 abort_ 同一个生命周期，一起清
    run_cost_ = 0;
    // 客户端靠 agent_start/agent_end 知道谁醒着、谁收工了——每帧都带 agent_id，
    // 所以不需要为「哪个 agent」再设计任何东西（ADR-0019 §5）
    broadcast("agent_start", nlohmann::json::object());
}

void Agent::finish_run(std::unique_lock<std::mutex> &busy)
{
    broadcast("agent_end", nlohmann::json{{"cost", run_cost_}});
    running_.store(false);
    const std::string summary = last_text(); // 通知带的正文，要在丢历史之前取
    {
        // 收件箱空了就把历史还给盘（ADR-0019 §7）。判据只有一条：**内存里那份是不是
        // 副本**。是副本就能丢，醒来重读一遍——走的就是 ensure_loaded()。立刻丢，不设
        // 「idle N 秒后丢」的定时器：那是又一个可调参数、又一个中间状态、又一个刚丢完
        // 就来消息的抖动。
        std::lock_guard<std::mutex> lk(mtx_);
        if (inbox_.empty())
        {
            messages_ = nlohmann::json::array();
            loaded_ = false;
        }
    }
    busy.unlock(); // on_done 要去动别的 agent，别攥着自己这把锁进去
    // 跑完了沿入边告诉关心我的那些 agent。这不是 hook 注册表——投递依据就是边，
    // 而边是投递方自己那头的一条数据（ADR-0019）
    if (pool_) pool_->on_done(id_, summary);
}

/* —— 三种消息入账。每种一个函数，loop 里就不必再摊开写 json —— */

/* 从 agent 外面来的一条。人发的、别的 agent 发的、完成通知、core 自己递的话头，
 * 全是 user——凡是从外面来的输入都是 user（ADR-0019 §5）。
 * 正文随帧走：客户端不能只靠"我刚才打了什么"渲染这一行，后三种它根本没打过。
 * 回放历史时走的是同一个帧，实时与翻历史因此长得一样（ADR-0020）。 */
void Agent::record_user(const std::string &text)
{
    record(nlohmann::json{
        {"role", "user"},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text}}})}});
    broadcast("message_start", nlohmann::json{{"role", "user"}, {"text", text}});
}

/* 收件箱里此刻攒着的全部，一口气取走。取用点只此一处、且在 turn 开头，于是「模型正在
 * 思考或正在跑工具时先别打断」不是一条规则，是没人来取的后果（ADR-0019 §5）。 */
void Agent::take_inbox()
{
    std::deque<std::string> incoming;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        incoming.swap(inbox_);
    }
    for (const std::string &m : incoming)
    {
        /* UserPromptSubmit（ADR-0024 §6）。**inject 附在这条消息后面，不进 system prompt**——
         * 那会让每一轮的 system prompt 都不一样，prompt cache 当场碎（ADR-0022 §3）。 */
        if (hooks_.empty())
        {
            record_user(m);
            continue;
        }
        nlohmann::json p{{"agent_id", id_}, {"cwd", workdir_}, {"prompt", m}};
        const HookOutcome h = hooks_.run(HookEvent::UserPromptSubmit, p, &abort_);
        if (h.deny)
        {
            // 拦下就是这条消息不进历史。报出来，不静默吞
            fprintf(stderr, "[hook] UserPromptSubmit 拦下一条消息：%s\n", h.reason.c_str());
            continue;
        }
        record_user(h.inject.empty() ? m : m + "\n\n" + h.inject);
    }
}

/* 把 thinking 块追加进 assistant content（thinking + signature）。
 * 思考块恒在正文/tool_use 块之前（协议约定顺序）。 */
static void append_thinking(nlohmann::json &content, const LlmOutcome &out)
{
    if (out.thinking.empty()) return;
    nlohmann::json b;
    b["type"] = "thinking";
    b["thinking"] = out.thinking;
    if (!out.thinking_signature.empty()) b["signature"] = out.thinking_signature;
    content.push_back(b);
}

/* 一次 LLM 产出记成一条 assistant 消息。块的顺序是协议约定的：thinking、正文、tool_use。
 * 正文与 tool_use 同时有是常事（"我看一下这个文件" + read），一条消息装得下。 */
void Agent::record_assistant(const LlmOutcome &out)
{
    nlohmann::json am;
    am["role"] = "assistant";
    am["content"] = nlohmann::json::array();
    append_thinking(am["content"], out);
    if (!out.text.empty())
        am["content"].push_back(nlohmann::json{{"type", "text"}, {"text", out.text}});
    for (const auto &tu : out.tool_uses)
    {
        nlohmann::json in = nlohmann::json::parse(tu.input, nullptr, false);
        am["content"].push_back(nlohmann::json{{"type", "tool_use"},
                                               {"id", tu.id},
                                               {"name", tu.name},
                                               {"input", in.is_discarded() ? nlohmann::json::object() : in}});
    }
    record(am);
}

/* 顺序执行这一批工具，每条结果即时入账。返回：模型打了 stop 没有。
 * 中断不由返回值表达——abort_ 调用方自己看得见。 */
bool Agent::run_tools(const LlmOutcome &out)
{
    bool stopped = false;
    size_t executed = 0;
    for (const auto &tu : out.tool_uses)
    {
        if (abort_.load()) break;
        broadcast("tool_execution_start", nlohmann::json{{"name", tu.name}, {"id", tu.id}});
        const nlohmann::json r = exe_.execute(tu.id, tu.name, tu.input);
        /* 三个字段每条返回路径都写齐了（Executor::execute），所以三个都直接取——
         * 给其中两个配默认值，只会在谁漏写的那天把这件事盖住。 */
        const bool is_error = r["isError"];
        const bool interrupted = r["interrupted"];
        nlohmann::json content = r["content"];
        // 认字段不认名字：哪个工具能收工是工具自己说的，loop 不抄一份工具表
        if (r.value("stop", false))
        {
            stopped = true;
            /* Stop（ADR-0024 §6）。**只观察，不改控制流**——出口仍然只有一个，
             * 这个出口只是多了一个旁观者。 */
            if (!hooks_.empty())
            {
                nlohmann::json p{{"agent_id", id_}, {"cwd", workdir_}};
                hooks_.run(HookEvent::Stop, p, &abort_);
            }
        }
        /* 帧里的 status 是协议契约（PROTOCOL.md），保持 int：0 或 1。
         * bash 的退出码从此不进这个帧——TUI 一直只判 != 0，行为一个字不变。 */
        broadcast("tool_execution_end", nlohmann::json{{"name", tu.name},
                                                       {"id", tu.id},
                                                       {"status", is_error ? 1 : 0},
                                                       {"interrupted", interrupted}});
        nlohmann::json tb;
        tb["type"] = "tool_result";
        tb["tool_use_id"] = tu.id;
        // 被中断的结果照实说，别混进"命令失败了"里——模型据此判断该不该重试，这两件事
        // 它的反应完全不同。手上那截输出仍然给它，那是真跑出来的
        if (interrupted)
            content.push_back(nlohmann::json{{"type", "text"}, {"text", "interrupted by user"}});
        tb["content"] = std::move(content);
        if (is_error || interrupted) tb["is_error"] = true;
        record(nlohmann::json{{"role", "user"}, {"content", nlohmann::json::array({tb})}});
        ++executed;
        if (interrupted) break;
    }
    // 没跑到的那几个也得有 tool_result。少一条，下一次请求里就是一个没人应答的 tool_use，
    // 端点直接 400——这个会话从此废了
    for (size_t i = executed; i < out.tool_uses.size(); ++i)
    {
        record(nlohmann::json{{"role", "user"},
                              {"content", nlohmann::json::array({nlohmann::json{{"type", "tool_result"},
                                                                                {"tool_use_id", out.tool_uses[i].id},
                                                                                {"content", nlohmann::json::array({{{"type", "text"}, {"text", "interrupted by user"}}})},
                                                                                {"is_error", true}}})}});
    }
    return stopped;
}

/* agent 主循环。**一层循环，一圈一个 turn**——turn 是这里唯一的重复单位。
 *
 * 一圈开头等的是「有活干」，两种：pending（上一圈没收工），或者收件箱里有东西。两种都
 * 没有就阻塞——那就是 idle（ADR-0019），不需要「空闲」这个状态。
 *
 * 一趟怎么结束：**模型调 stop 工具**，或者出错/被中断。「这次没调工具」不算收工——
 * 那只是它一句话说完了，不代表活干完了（stop.cpp）。
 *
 * 「一趟」（agent_start 到 agent_end、通知邻居、把历史还给盘）不是第二层循环，是
 * idle ⇄ busy 那条边沿：`busy.owns_lock()` 就是「此刻在一趟中间」，不另存状态位。
 *
 * 不设轮数上限：一个数字定不出"多少轮算跑飞了"——改个错字一圈，重构一个模块几十圈，
 * 两者都正常。刹车是下面那三处 continue：用户中断、llm_call 失败、端点空回答。 */
void Agent::loop()
{
    bool pending = false; // 还有活干：别等新消息，直接跑下一圈
    std::unique_lock<std::mutex> busy(run_mtx_, std::defer_lock);

    for (;;)
    {
        // 上一圈收工了而这一趟还开着 = 它到头了。摆在圈首不是延后：下面紧接着就是阻塞
        // 等待，收工帧先发出去了才等的
        if (busy.owns_lock() && !pending) finish_run(busy);

        // pending 写进等待条件里，于是「接着跑下一个 turn」与「等一条新消息」不是两条
        // 分支，是同一次等待的两种放行理由
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this, &pending] { return closing_ || pending || !inbox_.empty(); });
            if (closing_) return;
        }
        if (!busy.owns_lock()) start_run(busy);
        pending = false; // 默认这一圈就是最后一圈；只有下面两处才翻过来

        take_inbox();
        if (abort_.load())
        {
            broadcast("interrupted", nlohmann::json::object());
            continue;
        }

        broadcast("turn_start", nlohmann::json::object());
        LlmOutcome out;
        // 对话主链路走主模型；小模型档留给后续杂活调用点（标题/摘要）
        if (!llm_call(build_dialog(ModelTier::Main), out))
        {
            if (!abort_.load())
            {
                broadcast("turn_end",
                          nlohmann::json{{"error", out.error.empty() ? "llm_call failed" : out.error}});
                continue;
            }
            // 被中断的那次调用，它要的工具不算数：留着就是一批没人应答的 tool_use，
            // 下一次请求直接 400。手上那截正文/思考是真收到的，留下
            out.tool_uses.clear();
            if (!out.text.empty() || !out.thinking.empty()) record_assistant(out);
            broadcast("interrupted", nlohmann::json::object());
            continue;
        }
        run_cost_ += out.cost;

        // 一个字都没有：这不是一次"内容为空的成功"。空 text 块写进会话就是一块砖——下一轮
        // 原样回传，而端点拒收空 text 块，那个会话从此每轮都 400。报错、不落盘（ADR-0017）
        if (out.text.empty() && out.thinking.empty() && out.tool_uses.empty())
        {
            broadcast("turn_end",
                      nlohmann::json{{"error", "端点没有返回任何内容（HTTP 2xx 但流里一个事件都没有）"}});
            fprintf(stderr, "[agent] 空回答：不写入会话\n");
            continue;
        }
        record_assistant(out);

        if (out.tool_uses.empty())
        {
            broadcast("turn_end", nlohmann::json{{"stop_reason", out.stop_reason}});
            record_user(kStopReminder); // 没调工具：提醒完成即调用 stop，切勿臆想新任务
            pending = true;
            continue;
        }

        const bool stopped = run_tools(out);
        if (abort_.load())
        {
            broadcast("interrupted", nlohmann::json::object());
            continue;
        }
        broadcast("turn_end", nlohmann::json{{"tool_uses", (int)out.tool_uses.size()}});
        pending = !stopped;
    }
}

} // namespace realagent
