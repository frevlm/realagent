/*
 * test_compact.cpp — 压缩（ADR-0036）
 *
 * 前半是纯函数，后半对着 fake_endpoint.hpp 的假端点端到端跑。验的是这次改动里会坏的：
 *   - 读回历史从最后一条摘要开始，盘上压缩前的行留给回放，回放成 compact 帧
 *   - 抄本里摘要整条钉住：它是压缩之前的全部意图
 *   - compact_threshold 不在档上就启动失败
 *   - 写摘要的请求是一条带抄本的消息，/compact 的指令跟在里面
 *   - 用到阈值就在下一条消息之前自动压，新消息原样接在摘要后面
 *   - 端点报上下文超长：压一次、重跑；压完还超就报错收工，不打转
 *   - PreCompact 说 deny 就不压；PostCompact 拿到摘要
 */
#include <cstdio>
#include <string>

#include "agent/approval.hpp"
#include "agent/compact.hpp"
#include "agent/context.hpp"
#include "agent/verdict.hpp"
#include "fake_endpoint.hpp"
#include "llm/llm.hpp"

namespace fs = std::filesystem;
using namespace realagent;
using nlohmann::json;

static int failures = 0;
#define CHECK(cond, msg)                 \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            printf("  ok: %s\n", msg);   \
        }                                \
        else                             \
        {                                \
            printf("  FAIL: %s\n", msg); \
            ++failures;                  \
        }                                \
    } while (0)

static json text_msg(const char *role, const std::string &text)
{
    return json{{"role", role}, {"content", json::array({{{"type", "text"}, {"text", text}}})}};
}

/* 请求体里 messages 的各条正文，按顺序（system 不算） */
static std::vector<std::string> texts_of(const std::string &body)
{
    std::vector<std::string> out;
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return out;
    for (const json &m : j.value("messages", json::array()))
    {
        if (m.value("role", "") == "system") continue;
        const json &c = m["content"];
        out.push_back(c.is_string() ? c.get<std::string>() : c.dump());
    }
    return out;
}

/* 一个 agent 连同它的收帧器。每个用例一个 workdir：会话与 hooks.json 互不串 */
struct Rig {
    Frames f;
    ApprovalCoordinator approval;
    std::unique_ptr<Agent> a;

    Rig(Config &cfg, const Pricing &pricing, const fs::path &workdir, std::string session_id = {})
    {
        fs::create_directories(workdir);
        CoreContext ctx{.config = &cfg,
                        .pricing = &pricing,
                        .emit_fn = [this](const std::string &t, const std::string &p) { f.add(t, p); }};
        a = std::make_unique<Agent>(ctx, approval, workdir.string(), 1, nullptr, std::string(), std::string(),
                                    std::string(), std::move(session_id));
    }
};

int main()
{
    printf("== 读回从最后一条摘要开始 ==\n");
    {
        const json msgs = json::array({text_msg("user", "OLD"), text_msg("assistant", "A"),
                                       compact_message("S1"), text_msg("user", "MID"), compact_message("S2"),
                                       text_msg("user", "NEW")});
        CHECK(compact_begin(msgs) == 4, "两条摘要取最后那条");
        CHECK(compact_begin(json::array({text_msg("user", "x"), text_msg("assistant", "y")})) == 0,
              "没压过就从头读");
        CHECK(compact_begin(json::array({text_msg("assistant", std::string(kCompactTag) + "x")})) == 0,
              "助手说的话以标记开头不算摘要：摘要只能是 user 消息");
    }

    printf("== 回放 ==\n");
    {
        const json frames = Session::to_frames(json::array({text_msg("user", "OLD"), compact_message("SUMMARY")}));
        CHECK(frames.size() == 2 && frames[0]["type"] == "message_start", "压缩前的消息照常回放");
        CHECK(frames[1]["type"] == "compact" && frames[1]["data"]["text"] == "SUMMARY",
              "摘要回放成 compact 帧，正文去掉标记");
    }

    printf("== 抄本钉住摘要 ==\n");
    {
        const std::string summary(kVerdictLineCap * 3, 's');
        json msgs = json::array({compact_message(summary)});
        for (int i = 0; i < 100; ++i) msgs.push_back(text_msg("assistant", std::string(1000, 'x')));
        const std::string t = run_transcript(msgs, 0, 20000);
        CHECK(t.starts_with(std::string(kCompactTag) + summary + "\n"), "摘要整条进抄本，不按行截");
        CHECK(t.size() <= 20000 + 64, "其余照预算丢");
    }

    printf("== 上下文超长的认法 ==\n");
    {
        CHECK(context_overflow(400, R"({"error":{"message":"prompt is too long: 210000 tokens > 200000 maximum"}})"),
              "Anthropic 的原话");
        CHECK(context_overflow(400, R"({"error":{"code":"context_length_exceeded"}})"), "OpenAI 的错误码");
        CHECK(context_overflow(400, R"({"error":{"message":"This model's maximum context length is 65536 tokens"}})"),
              "OpenAI 兼容端点的原话");
        CHECK(context_overflow(400, R"({"error":{"message":"invalid tool schema"}})") == false, "别的 400 不算");
        CHECK(context_overflow(500, "maximum context length") == false, "5xx 不算：那是端点自己坏了");
    }

    const fs::path home = fs::temp_directory_path() / ("realagent-compact-test-" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);

    printf("== compact_threshold 以 5 为一档 ==\n");
    {
        auto c = reload(home, json::object());
        CHECK(c && c->get_json("compact_threshold") == 85, "默认 85");
        std::ofstream(home / ".realagent" / "settings.json") << R"({"compact_threshold": 87})";
        const auto bad = Config::load();
        CHECK(!bad && has(bad.error(), "compact_threshold"), "87 不在档上：启动失败，报出键名");
        std::ofstream(home / ".realagent" / "settings.json") << R"({"compact_threshold": 45})";
        CHECK(!Config::load(), "低于 50 启动失败");
        std::ofstream(home / ".realagent" / "settings.json") << R"({"compact_threshold": 100})";
        CHECK(Config::load().has_value(), "100 在档上");
    }

    FakeEndpoint ep;
    auto cfg = reload(home, {{"protocol", "revlm"},
                             {"base_url", ep.base_url()},
                             {"api_key", "k"},
                             {"model", "m-main"},
                             {"permission", "allow-all"}});
    if (!cfg) return 1;
    std::ofstream(home / ".realagent" / "models.json")
        << R"([{"name":"m-main","owned_by":"t","context":1000,"pricing":{}}])";
    std::string price_err;
    const Pricing pricing = Pricing::load(*cfg, &price_err);
    CHECK(price_err.empty() && pricing.context("m-main") == 1000, "模型表报出窗口");

    printf("== 手动压缩 ==\n");
    {
        const fs::path wd = home / "manual";
        std::string session;
        {
            Rig r(*cfg, pricing, wd);
            session = r.a->session_id();
            ep.main_says({"FIRST_ANSWER", "THE_SUMMARY"});
            r.a->post("FIRST_ASK");
            CHECK(wait_end(r.f), "第一趟收工");
            const size_t before = ep.main_requests().size();
            r.a->request_compact("KEEP_PATHS");
            CHECK(wait_end(r.f, 2), "idle 时 /compact 自己开一趟、收工");

            const std::vector<std::string> sent = texts_of(ep.main_requests().at(before));
            CHECK(sent.size() == 1 && has(sent[0], "<transcript>") && has(sent[0], "[user] FIRST_ASK") &&
                      has(sent[0], "[assistant] FIRST_ANSWER"),
                  "写摘要的请求是一条消息，历史转成抄本放在里面");
            CHECK(has(sent[0], "KEEP_PATHS"), "/compact 的指令跟在里面");
            CHECK(has(r.f.last("compact"), "THE_SUMMARY") && r.f.count("compact_start") == 1,
                  "推 compact_start 与带摘要的 compact 帧");
            CHECK(r.f.count("message_update") == 1 && has(r.f.joined("message_update"), "FIRST_ANSWER"),
                  "摘要不当正文推：正文只有第一趟那段");

            const json h = history(*r.a);
            CHECK(h.size() == 3 && h[2] == compact_message("THE_SUMMARY"), "摘要追加进会话文件，之前的行不动");
        }
        // 换一个 agent 接着这段对话说：只看得到摘要与之后的
        Rig r(*cfg, pricing, wd, session);
        ep.main_says({"NEXT_ANSWER"});
        r.a->post("NEXT_ASK");
        CHECK(wait_end(r.f), "接着说的那趟收工");
        const std::vector<std::string> sent = texts_of(ep.main_requests().back());
        CHECK(sent.size() == 2 && has(sent[0], "THE_SUMMARY") && has(sent[1], "NEXT_ASK"),
              "读回从摘要开始：压缩前的不再发给模型");
    }

    printf("== 用到阈值自动压 ==\n");
    {
        Rig r(*cfg, pricing, home / "auto");
        ep.main_prompt_tokens(900); // 窗口 1000、阈值 85%：900 + 1 已过线
        ep.main_says({"FIRST_ANSWER", "AUTO_SUMMARY", "SECOND_ANSWER"});
        r.a->post("FIRST_ASK");
        CHECK(wait_end(r.f), "第一趟收工");
        const size_t before = ep.main_requests().size();
        r.a->post("SECOND_ASK");
        CHECK(wait_end(r.f, 2), "第二趟收工");
        ep.main_prompt_tokens(0);

        const std::vector<std::string> reqs = ep.main_requests();
        CHECK(reqs.size() - before == 2, "第二趟先压缩、再答：主模型被问了两次");
        const std::vector<std::string> sent = texts_of(reqs.back());
        CHECK(sent.size() == 2 && has(sent[0], "AUTO_SUMMARY") && has(sent[1], "SECOND_ASK"),
              "新消息原样接在摘要后面");
        CHECK(has(r.f.last("compact_start"), "\"trigger\":\"auto\""), "trigger 是 auto");
    }

    printf("== 端点报上下文超长 ==\n");
    {
        Rig r(*cfg, pricing, home / "overflow");
        ep.main_says({"FIRST_ANSWER", FakeEndpoint::kOverflow, "OVERFLOW_SUMMARY", "RETRIED_ANSWER"});
        r.a->post("FIRST_ASK");
        CHECK(wait_end(r.f), "第一趟收工");
        const size_t before = ep.main_requests().size();
        r.a->post("SECOND_ASK");
        CHECK(wait_end(r.f, 2), "第二趟收工");

        const std::vector<std::string> reqs = ep.main_requests();
        CHECK(reqs.size() - before == 3, "超长 → 压缩 → 重跑，三次");
        CHECK(has(reqs[before + 1], "SECOND_ASK"), "超长那一圈的消息也压进摘要");
        const std::vector<std::string> retried = texts_of(reqs.back());
        CHECK(retried.size() == 1 && has(retried[0], "OVERFLOW_SUMMARY"), "重跑的那次只剩摘要");
        CHECK(has(r.f.joined("message_update"), "RETRIED_ANSWER") && has(r.f.last("turn_end"), "stop_reason"),
              "重跑答上了，这一趟正常收尾");
    }

    printf("== 压完还超长：报错收工，不打转 ==\n");
    {
        Rig r(*cfg, pricing, home / "overflow-twice");
        ep.main_says({"FIRST_ANSWER", FakeEndpoint::kOverflow, "SUMMARY", FakeEndpoint::kOverflow, "NEVER"});
        r.a->post("FIRST_ASK");
        CHECK(wait_end(r.f), "第一趟收工");
        const size_t before = ep.main_requests().size();
        r.a->post("SECOND_ASK");
        CHECK(wait_end(r.f, 2), "第二趟收工");
        CHECK(ep.main_requests().size() - before == 3, "只压一次：超长、压缩、再超长就停");
        CHECK(has(r.f.last("turn_end"), "prompt is too long"), "报的是端点的原话");
    }

    printf("== PreCompact / PostCompact ==\n");
    {
        const fs::path wd = home / "hooks";
        const fs::path seen = home / "post-compact.json";
        fs::create_directories(wd / ".realagent" / "hooks");
        std::ofstream(wd / ".realagent" / "hooks" / "hooks.json")
            << json{{"hooks",
                     {{"PreCompact",
                       {{{"matcher", "manual"}, {"hooks", {{{"command", "echo NO_COMPACT >&2; exit 2"}}}}}}},
                      {"PostCompact", {{{"hooks", {{{"command", "cat > '" + seen.string() + "'"}}}}}}}}}}
                   .dump();

        Rig r(*cfg, pricing, wd);
        ep.main_says({"FIRST_ANSWER", FakeEndpoint::kOverflow, "HOOKED_SUMMARY", "AFTER"});
        r.a->post("FIRST_ASK");
        CHECK(wait_end(r.f), "第一趟收工");
        const size_t before = ep.main_requests().size();
        r.a->request_compact("");
        CHECK(wait_end(r.f, 2), "被拦的那趟也收工");
        CHECK(ep.main_requests().size() == before && has(r.f.last("turn_end"), "NO_COMPACT"),
              "PreCompact deny：一个请求都不发，报出 hook 的理由");

        // matcher 只拦 manual：溢出那次照压，PostCompact 拿到摘要
        r.a->post("SECOND_ASK");
        CHECK(wait_end(r.f, 3), "第三趟收工");
        std::ifstream in(seen);
        const json got = json::parse(in, nullptr, false);
        CHECK(got.is_object() && got["hook_event_name"] == "PostCompact" && got["trigger"] == "overflow" &&
                  got["compact_summary"] == "HOOKED_SUMMARY",
              "PostCompact 的 stdin 带 trigger 与摘要");
    }

    fs::remove_all(home);
    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
