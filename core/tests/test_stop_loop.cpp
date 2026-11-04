/*
 * test_stop_loop.cpp — 出口那条路，端到端（ADR-0025、ADR-0032）
 *
 * 对手是 fake_endpoint.hpp 里的假端点，整条路走得通，不必真花钱。
 *
 * 验的全是这次改动里会坏的：
 *   - 主模型没调工具**不等于收工**：decision_model 说没干完就打回，主模型接着跑，**小模型一次不问**
 *   - 打回那条消息进历史，带 supervisor 标记（下一次判定要认得出它不是用户说的）
 *   - decision_model 说干完了才问小模型写 recap，recap 落在 agent_end 帧里
 *   - **recap 那次调用的正文一个字都不往客户端推**——那是给 recap 自己用的
 *   - 一直说没干完时，连着三轮没跑工具就强制收工（kMaxStall）
 *   - 没配 decision_model、或端点不是 revlm：不问 System One，行为退回这个功能之前
 */
#include <cstdio>
#include <string>

#include "agent/approval.hpp"
#include "agent/context.hpp"
#include "agent/verdict.hpp"
#include "fake_endpoint.hpp"

namespace fs = std::filesystem;
using namespace realagent;

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

int main()
{
    const fs::path home =
        fs::temp_directory_path() / ("realagent-stoploop-test-" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);

    FakeEndpoint ep;
    const nlohmann::json revlm{{"protocol", "revlm"},
                               {"base_url", ep.base_url()},
                               {"api_key", "k"},
                               {"model", "m-main"},
                               {"small_model", "m-small"},
                               {"decision_model", "m-decide"},
                               {"permission", "allow-all"}};
    auto cfg = reload(home, revlm);
    if (!cfg) return 1;

    printf("== System One 说没干完：打回，主模型接着跑，小模型一次不问 ==\n");
    {
        Frames f;
        CoreContext ctx{.config = &*cfg,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        ep.main_says({"MAIN_FIRST", "MAIN_SECOND"});
        ep.decision_says({0.2, 0.9});
        ep.small_says({"RECAP_DONE"});

        Agent a(ctx, approval, home.string(), 1);
        a.post("干活");
        CHECK(wait_end(f), "收工了（等到 agent_end）");

        CHECK(ep.main_calls() == 2, "主模型跑了两轮——第一轮没调工具但没收工，是被打回的");
        CHECK(ep.decision_calls() == 2, "System One 问了两次，一轮一次");
        CHECK(ep.small_calls() == 1, "小模型只在收工那一轮写 recap，打回那轮没问");

        const std::string asked = ep.last_decision_request();
        const nlohmann::json body = nlohmann::json::parse(asked.substr(asked.find("\r\n\r\n") + 4), nullptr, false);
        CHECK(has(asked, "POST /v1/systemone") && has(asked, "Authorization: Bearer k"), "POST /v1/systemone，带 Bearer");
        CHECK(body.is_object() && body.size() == 3 && body["model"] == "m-decide", "体里是 model / state / questions");
        CHECK(body["state"]["user"] == "干活" && body["state"]["assistant"] == "MAIN_SECOND",
              "state 是这一趟 user 的话与最后一条助手正文（打回的不算 user）");

        const nlohmann::json h = history(a);
        int kicked = 0;
        for (const auto &m : h)
            if (m.value("role", "") == "user")
                for (const auto &b : m["content"])
                    if (b.value("type", "") == "text" && b.value("text", "") == kKickback) ++kicked;
        CHECK(kicked == 1, "打回那条进了历史，一次打回一条");
        CHECK(std::string(kKickback).rfind(kSupervisorTag, 0) == 0,
              "打回那条带 supervisor 标记，下一次判定认得出它不是用户说的");

        const std::string ended = f.last("agent_end");
        CHECK(has(ended, "RECAP_DONE"), "收工那一帧带 recap");

        const std::string deltas = f.joined("message_update");
        CHECK(has(deltas, "MAIN_FIRST") && has(deltas, "MAIN_SECOND"), "主模型的正文照推");
        CHECK(f.count("message_update") == 2, "recap 那次调用的正文一个字都没推给客户端：只有主模型的两段");
    }

    printf("== System One 一直说没干完：连着三轮没跑工具就强制收工 ==\n");
    {
        Frames f;
        CoreContext ctx{.config = &*cfg,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int main_before = ep.main_calls(), decision_before = ep.decision_calls(), small_before = ep.small_calls();
        ep.main_says({"STILL_TALKING"});
        ep.decision_says({0.1});
        ep.small_says({"RECAP_FORCED"});

        Agent a(ctx, approval, home.string(), 3);
        a.post("干活");
        CHECK(wait_end(f), "还是收工了——不设轮数上限，但推不动了要认");
        CHECK(ep.main_calls() - main_before == 3,
              "主模型跑了三轮就打住：打回两次，第三次判定强制收工（kMaxStall）");
        CHECK(ep.decision_calls() - decision_before == 3, "System One 问了三次");
        CHECK(ep.small_calls() - small_before == 1 && has(f.last("agent_end"), "RECAP_FORCED"),
              "强制收工也是收工：小模型写一次 recap，邻居照样知道这一趟干了什么");
    }

    printf("== 没配 decision_model：不问 System One，没调工具就算干完了 ==\n");
    {
        // **不回落**主模型或小模型（ADR-0010）。也去掉 small_model：什么都不问
        nlohmann::json s = revlm;
        s.erase("decision_model");
        s.erase("small_model");
        auto c2 = reload(home, s);
        if (!c2) return 1;

        Frames f;
        CoreContext ctx{.config = &*c2,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int decision_before = ep.decision_calls(), small_before = ep.small_calls();
        ep.main_says({"ONLY_ANSWER"});

        Agent a(ctx, approval, home.string(), 2);
        a.post("说句话");
        CHECK(wait_end(f), "收工了");
        CHECK(ep.decision_calls() == decision_before, "System One 一次没问");
        CHECK(ep.small_calls() == small_before, "小模型一次没问");
        CHECK(has(f.last("agent_end"), "\"recap\":\"\""), "没有小模型就没有 recap，字段仍在（帧形状恒定）");
    }

    printf("== 端点不是 revlm：decision_model 配了也不问 ==\n");
    {
        nlohmann::json s = revlm;
        s["protocol"] = "openai-chat";
        s["base_url"] = ep.base_url() + "/v1";
        auto c3 = reload(home, s);
        if (!c3) return 1;

        Frames f;
        CoreContext ctx{.config = &*c3,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int decision_before = ep.decision_calls(), small_before = ep.small_calls();
        ep.main_says({"ONLY_ANSWER"});
        ep.small_says({"RECAP_PLAIN"});

        Agent a(ctx, approval, home.string(), 4);
        a.post("说句话");
        CHECK(wait_end(f), "收工了");
        CHECK(ep.decision_calls() == decision_before, "System One 一次没问");
        CHECK(ep.small_calls() - small_before == 1 && has(f.last("agent_end"), "RECAP_PLAIN"), "小模型照写 recap");
    }

    printf("== 配了 effort 的 messages 渠道：量级进主模型请求，只有签名的思考块照样落盘 ==\n");
    {
        nlohmann::json s = revlm;
        s["model"] = "m-claude";
        s["effort"] = "high";
        s.erase("decision_model");
        s.erase("small_model");
        auto c4 = reload(home, s);
        if (!c4) return 1;

        Frames f;
        CoreContext ctx{.config = &*c4,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        Agent a(ctx, approval, home.string(), 5);
        a.post("想一想再答");
        CHECK(wait_end(f), "收工了");

        nlohmann::json sent = nlohmann::json::parse(ep.last_claude_request(), nullptr, false);
        CHECK(sent.is_object() && sent["output_config"]["effort"] == "high", "主模型请求带 output_config.effort");

        nlohmann::json h = history(a);
        const nlohmann::json want{{"type", "thinking"}, {"thinking", ""}, {"signature", "SIG_OMITTED"}};
        CHECK(h.size() == 2 && h[1]["content"][0] == want, "助手消息的第一块是那个思考块，签名在");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
