#include "agent/agent.hpp"

#include "agent/agents.hpp"

#include <curl/curl.h>

#include <cstdio>

namespace realagent {

using nlohmann::json;

namespace {

/* 每帧带上是哪段对话的：session_id 是自己的，root 是用户看见的那段（ADR-0029）。
 * 载荷都是 dump 出来的 JSON 对象，往开头那个 { 后面插两个键即可，不必再解析一遍。 */
EmitFn stamped(EmitFn inner, const std::string &session_id, const std::string &root)
{
    if (!inner) return inner;
    std::string head = json{{"session_id", session_id}, {"root", root}}.dump();
    head.pop_back(); // 去掉收尾的 }
    return [inner = std::move(inner), head](const std::string &type, const std::string &payload) {
        inner(type, payload.size() <= 2 ? head + "}" : head + "," + payload.substr(1));
    };
}

json text_block(std::string text) { return json{{"type", "text"}, {"text", std::move(text)}}; }

json tool_result_msg(const std::string &id, json content, bool is_error)
{
    json tb{{"type", "tool_result"}, {"tool_use_id", id}, {"content", std::move(content)}};
    if (is_error) tb["is_error"] = true;
    return json{{"role", "user"}, {"content", json::array({std::move(tb)})}};
}

/* 打回时递给主模型的话头。前缀让下一次判定认得出这是裁判说的，不是用户说的。 */
std::string kickback(const std::string &reason)
{
    return std::string(kSupervisorTag) + (reason.empty() ? "This run is not finished yet." : reason) +
           " Keep working on it. Do not start tasks the user did not request.";
}

/* 连着几次打回、主模型仍一个工具都没调，就强制收工：再问只是两个模型互相说话花钱。 */
constexpr int kMaxStall = 3;

} // namespace

Agent::Agent(CoreContext ctx, ApprovalCoordinator &approval, std::string workdir, int id,
             Agents *pool, std::string session_dir, std::string root, std::string def_body,
             std::string session_id)
    : ctx_(std::move(ctx)), pool_(pool), id_(id), workdir_(std::move(workdir)),
      mcp_(ctx_.mcp ? ctx_.mcp->open(workdir_, ctx_.config->get_json("mcp_http_bridge"))
                    : McpHub::Lease{}),
      hooks_(Hooks::scan(workdir_)), agent_defs_(scan_agent_defs(workdir_)),
      exe_(ctx_, approval, workdir_, pool_, id_, &mcp_, &hooks_, &agent_defs_),
      skills_(scan_skills(workdir_)),
      session_dir_(session_dir.empty() ? sessions_dir(workdir_) : std::move(session_dir)),
      session_(session_dir_, std::move(session_id)), loaded_(false), def_body_(std::move(def_body))
{
    // loaded_ 为 false：第一次跑时从盘上读，盘上没有就是空的——新开与接着说是同一条路
    root_ = root.empty() ? session_.id() : std::move(root);
    ctx_.emit_fn = stamped(std::move(ctx_.emit_fn), session_.id(), root_);
    for (const std::string &e : plugin_errors()) fprintf(stderr, "[plugin] %s\n", e.c_str());
    if (!mcp_.tools.empty())
        fprintf(stderr, "[mcp] agent %d: %zu servers, %zu tools\n", id_, mcp_.conns.size(),
                mcp_.tools.size());
    // 会阻塞建 agent，每个 hook 最多它自己的 timeout
    session_start_hook(Session::exists(session_dir_, session_.id()) ? "resume" : "startup");
    loop_ = std::thread([this] { loop(); });
}

Agent::~Agent()
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        closing_ = true;
    }
    cv_.notify_all();
    interrupt();
    if (loop_.joinable()) loop_.join();
}

std::vector<std::string> Agent::plugin_errors() const
{
    std::vector<std::string> all = mcp_.errors;
    all.insert(all.end(), hooks_.errors().begin(), hooks_.errors().end());
    return all;
}

void Agent::post(std::string message, bool notice)
{
    {
        std::lock_guard<std::mutex> lk(mtx_);
        inbox_.push_back({std::move(message), notice});
    }
    cv_.notify_one();
}

void Agent::ensure_loaded()
{
    if (loaded_) return;
    if (!Session::read(session_dir_, session_.id(), messages_))
        messages_ = json::array(); // 还没写过盘
    loaded_ = true;
}

void Agent::record(json msg)
{
    session_.append(msg);
    messages_.push_back(std::move(msg));
}

void Agent::interrupt()
{
    abort_.store(true);
    exe_.interrupt();
}

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

/* 是谁的帧由 emit_fn 自己盖章（stamped），这里只管载荷是个对象。 */
void Agent::broadcast(const std::string &type, const json &payload)
{
    if (!ctx_.emit_fn) return;
    ctx_.emit_fn(type, (payload.is_object() ? payload : json::object()).dump());
}

/* —— LLM 调用 —— */

namespace {

struct StreamCtx {
    Agent *self;
    CURL *curl;
    SseParser parser;
    LlmOutcome *out;
    std::string model;
    bool silent;
    bool parse_failed = false;
    long status = 0;
    std::string error_body; // 非 2xx 时的响应体
};

int curl_progress_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const std::atomic<bool> *>(clientp)->load() ? 1 : 0;
}

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *s = static_cast<StreamCtx *>(userdata);
    const size_t n = size * nmemb;
    // 先看状态码：4xx/5xx 的响应体不是 SSE，喂给解析器只会得到"成功但空"
    if (s->status == 0) curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &s->status);
    if (s->status >= 400)
    {
        if (s->error_body.size() < 8192) s->error_body.append(ptr, n);
        return n;
    }
    const bool ok = s->parser.feed(std::string_view(ptr, n), [s](std::string_view t, const json &ev) {
        s->self->on_llm_event(t, ev, *s->out, s->model, s->silent);
    });
    if (ok) return n;
    s->parse_failed = true;
    return 0; // 返回 < n 让 curl 中止传输
}

} // namespace

void Agent::on_llm_event(std::string_view type, const json &ev, LlmOutcome &out,
                         const std::string &model, bool silent)
{
    if (type == "message_update")
        out.text += ev["delta"].get<std::string>();
    else if (type == "thinking_start")
        out.thinking_signature = ev["signature"];
    else if (type == "thinking_update")
        out.thinking += ev["delta"].get<std::string>();
    else if (type == "tool_use")
        out.tool_uses.push_back(
            {ev["id"], ev["name"], ev["input"].is_null() ? "{}" : ev["input"].dump()});
    else if (type == "stop")
        out.stop_reason = ev["reason"];
    else if (type == "usage")
    {
        // usage 不上传，换成钱再报；算不出就不报
        const double cost = ctx_.pricing ? ctx_.pricing->cost(model, ev) : 0;
        if (cost <= 0) return;
        out.cost = cost;
        broadcast("status_update", json{{"cost", run_cost_ + cost}});
        return;
    }
    if (!silent && type != "tool_use" && type != "stop") broadcast(std::string(type), ev);
}

bool Agent::llm_call(const json &dialog, LlmOutcome &out, bool silent)
{
    const auto fail = [&out](std::string why) {
        out.error = std::move(why);
        fprintf(stderr, "[agent] %s\n", out.error.c_str());
        return false;
    };
    // 别的入口（send_message、完成通知）也会走到这里，所以每次都查
    if (std::string e = endpoint_config_error(*ctx_.config); !e.empty()) return fail(std::move(e));

    const HttpRequest req = build_request(*ctx_.config, dialog);
    if (!req.url.starts_with("http")) return fail("base_url 不像个 URL（当前请求 URL: " + req.url + "）");

    CURL *curl = curl_easy_init();
    if (!curl) return fail("curl 初始化失败");
    StreamCtx s{.self = this,
                .curl = curl,
                .parser = SseParser(*protocol_from(ctx_.config->get("protocol"))),
                .out = &out,
                .model = dialog.value("model", std::string()),
                .silent = silent};
    curl_slist *hdrs = nullptr;
    for (const auto &h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &s);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);

    // 真因优先：HTTP 状态 > 解析失败（它引起的 WRITE_ERROR 不是真因）> 传输错误
    if (std::string e = http_status_error(s.status, s.error_body); !e.empty()) return fail(std::move(e));
    if (s.parse_failed) return fail("解析响应失败（见 core 日志）");
    if (rc != CURLE_OK) return fail(std::string("curl 失败: ") + curl_easy_strerror(rc));
    return true;
}

std::string Agent::system_prompt() const
{
    const auto section = [](const std::string &s) { return s.empty() ? s : "\n\n" + s; };
    return "You are a helpful coding agent. Your agent id is " + std::to_string(id_) +
           ". Your working directory is " + workdir_ +
           ".\n"
           "You run in an autonomous loop: keep calling tools until the user's request is fully "
           "handled. When you have no next action left, write your final answer and call no tools. "
           "A supervisor then checks whether the run is really finished and hands it back to you if "
           "it is not. Do not invent tasks the user did not request." +
           skills_prompt(skills_) + agent_defs_prompt(agent_defs_) + section(def_body_) +
           section(session_context_);
}

json Agent::build_dialog(ModelTier tier) const
{
    json tools = tool_defs();
    for (const json &t : mcp_.tools) tools.push_back(t);
    for (auto &t : tools) t.erase("_core");
    return json{{"model", ctx_.config->model(tier)},
                {"system", system_prompt()},
                {"tools", std::move(tools)},
                {"messages", messages_}};
}

void Agent::session_start_hook(const std::string &source)
{
    session_context_ = hooks_.run(HookEvent::SessionStart,
                                  {{"matcher_key", source},
                                   {"agent_id", id_},
                                   {"cwd", workdir_},
                                   {"source", source},
                                   {"session_id", session_.id()}},
                                  &abort_)
                           .inject;
}

/* —— 一趟的边沿 —— */

void Agent::start_run(std::unique_lock<std::mutex> &busy)
{
    busy.lock();
    running_.store(true);
    ensure_loaded();
    run_begin_ = messages_.size();
    recap_.clear();
    asked_ = false;
    stall_ = 0;
    run_cost_ = 0;
    abort_.store(false);
    exe_.reset();
    broadcast("agent_start", json::object());
}

void Agent::finish_run(std::unique_lock<std::mutex> &busy)
{
    broadcast("agent_end", json{{"cost", run_cost_}, {"recap", recap_}});
    running_.store(false);
    const std::string summary = recap_.empty() ? last_text() : recap_; // 丢历史之前取
    {
        // 收件箱空了就把历史还给盘：内存里那份只是盘上的副本
        std::lock_guard<std::mutex> lk(mtx_);
        if (inbox_.empty())
        {
            messages_ = json::array();
            loaded_ = false;
        }
    }
    busy.unlock(); // on_done 要动别的 agent，别攥着自己的锁
    // 两种收工不通知邻居：只由完成通知唤醒的那一趟（那只是回声，环上会无限互相唤醒）；
    // 被中断的那一趟（没跑完，而且用户正要停下整段对话，通知会把刚停下的又叫醒）
    if (pool_ && asked_ && !abort_.load()) pool_->on_done(id_, summary);
}

/* —— 入账 —— */

/* 从外面来的输入一律是 user。正文随帧走，回放历史与实时看长得一样。 */
void Agent::record_user(const std::string &text)
{
    record(json{{"role", "user"}, {"content", json::array({text_block(text)})}});
    broadcast("message_start", json{{"role", "user"}, {"text", text}});
}

/* 收件箱只在 turn 开头取，于是模型思考或跑工具时不会被打断。 */
void Agent::take_inbox()
{
    std::deque<Mail> incoming;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        incoming.swap(inbox_);
    }
    for (const auto &[m, notice] : incoming)
    {
        // UserPromptSubmit 的注入附在消息后面，不进 system prompt（那会打碎 prompt cache）
        const HookOutcome h = hooks_.run(HookEvent::UserPromptSubmit,
                                         {{"agent_id", id_}, {"cwd", workdir_}, {"prompt", m}}, &abort_);
        if (h.deny)
        {
            fprintf(stderr, "[hook] UserPromptSubmit 拦下一条消息：%s\n", h.reason.c_str());
            continue;
        }
        asked_ = asked_ || !notice;
        record_user(h.inject.empty() ? m : m + "\n\n" + h.inject);
    }
}

/* 块序由协议定：thinking、正文、tool_use。 */
void Agent::record_assistant(const LlmOutcome &out)
{
    json content = json::array();
    if (!out.thinking.empty())
    {
        json b{{"type", "thinking"}, {"thinking", out.thinking}};
        if (!out.thinking_signature.empty()) b["signature"] = out.thinking_signature;
        content.push_back(std::move(b));
    }
    if (!out.text.empty()) content.push_back(text_block(out.text));
    for (const auto &tu : out.tool_uses)
    {
        json in = json::parse(tu.input, nullptr, false);
        content.push_back(json{{"type", "tool_use"},
                               {"id", tu.id},
                               {"name", tu.name},
                               {"input", in.is_discarded() ? json::object() : in}});
    }
    record(json{{"role", "assistant"}, {"content", std::move(content)}});
}

/* 顺序执行。每个 tool_use 都必须有一条 tool_result，否则下一次请求端点直接 400——
 * 被中断后没跑到的也补一条。 */
void Agent::run_tools(const LlmOutcome &out)
{
    for (const auto &tu : out.tool_uses)
    {
        if (abort_.load())
        {
            record(tool_result_msg(tu.id, json::array({text_block("interrupted by user")}), true));
            continue;
        }
        broadcast("tool_execution_start", json{{"name", tu.name}, {"id", tu.id}});
        const json r = exe_.execute(tu.id, tu.name, tu.input);
        const bool is_error = r["isError"], interrupted = r["interrupted"];
        broadcast("tool_execution_end", json{{"name", tu.name},
                                             {"id", tu.id},
                                             {"status", is_error ? 1 : 0},
                                             {"interrupted", interrupted}});
        // 被中断要照实说：模型对"命令失败"和"被用户打断"的反应完全不同
        json content = r["content"];
        if (interrupted) content.push_back(text_block("interrupted by user"));
        record(tool_result_msg(tu.id, std::move(content), is_error || interrupted));
    }
}

/* —— 收工判定（ADR-0025）——
 * 把这一趟的抄本交给小模型；请求不带工具，正文不推给客户端。
 * 没配小模型、调用失败都判收工：判不出来不该把人扣在循环里花钱。 */
StopVerdict Agent::judge()
{
    const std::string model = ctx_.config->model(ModelTier::Small);
    if (model.empty()) return {}; // 没有裁判：不回落主模型
    const json prompt = text_block(stop_verdict_prompt(run_transcript(messages_, run_begin_)));
    const json d{{"model", model},
                 {"system", stop_verdict_system()},
                 {"messages", json::array({json{{"role", "user"}, {"content", json::array({prompt})}}})}};
    LlmOutcome out;
    const bool ok = llm_call(d, out, /*silent=*/true);
    run_cost_ += out.cost;
    if (!ok)
    {
        fprintf(stderr, "[agent] 收工判定失败，按收工处理：%s\n", out.error.c_str());
        return {};
    }
    return parse_stop_verdict(out.text);
}

/* 一圈一个 turn。等的是「有活干」：pending（上一圈没收工）或收件箱非空；都没有就是 idle。
 * 刹车：用户中断、LLM 调用失败、空回答、kMaxStall。不设轮数上限。 */
void Agent::loop()
{
    bool pending = false;
    std::unique_lock<std::mutex> busy(run_mtx_, std::defer_lock);
    const auto interrupted = [this] {
        if (!abort_.load()) return false;
        broadcast("interrupted", json::object());
        return true;
    };

    for (;;)
    {
        if (busy.owns_lock() && !pending) finish_run(busy);
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this, &pending] { return closing_ || pending || !inbox_.empty(); });
            if (closing_) return;
        }
        if (!busy.owns_lock()) start_run(busy);
        pending = false;

        take_inbox();
        if (interrupted()) continue;

        broadcast("turn_start", json::object());
        LlmOutcome out;
        if (!llm_call(build_dialog(ModelTier::Main), out))
        {
            if (!abort_.load())
            {
                broadcast("turn_end", json{{"error", out.error.empty() ? "llm_call failed" : out.error}});
                continue;
            }
            // 被中断：要的工具不算数（留着就是没人应答的 tool_use），收到的正文留下
            out.tool_uses.clear();
            if (!out.text.empty() || !out.thinking.empty()) record_assistant(out);
            interrupted();
            continue;
        }
        run_cost_ += out.cost;

        // 空回答不落盘：空 text 块会让这个会话之后每轮都 400
        if (out.text.empty() && out.thinking.empty() && out.tool_uses.empty())
        {
            broadcast("turn_end", json{{"error", "端点没有返回任何内容（HTTP 2xx 但流里一个事件都没有）"}});
            fprintf(stderr, "[agent] 空回答：不写入会话\n");
            continue;
        }
        record_assistant(out);

        if (!out.tool_uses.empty())
        {
            run_tools(out);
            if (interrupted()) continue;
            broadcast("turn_end", json{{"tool_uses", (int)out.tool_uses.size()}});
            stall_ = 0;
            pending = true;
            continue;
        }

        // 主模型没有下一步动作了：收不收工问裁判
        broadcast("turn_end", json{{"stop_reason", out.stop_reason}});
        const StopVerdict v = judge();
        if (interrupted()) continue;
        if (!v.done && ++stall_ < kMaxStall)
        {
            record_user(kickback(v.reason));
            pending = true;
            continue;
        }
        if (!v.done) fprintf(stderr, "[agent] 连着 %d 轮没跑一个工具，强制收工\n", kMaxStall);
        recap_ = v.recap;
        hooks_.run(HookEvent::Stop, {{"agent_id", id_}, {"cwd", workdir_}}, &abort_); // 只观察
    }
}

} // namespace realagent
