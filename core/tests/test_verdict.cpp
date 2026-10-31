/*
 * test_verdict.cpp — 收工判定的纯函数（ADR-0025、ADR-0032）
 *
 * 这里不打 LLM 调用，也不起 agent：judge() 要的是网络与配置，而会坏的不是那一次
 * 请求，是这几件事——
 *   - 抄本里 user 消息一条不丢（那是意图，丢了 recap 就没法写「用户要的」）
 *   - 抄本里认得出裁判自己上次说的话（不标的话它会读成用户的新要求，然后催第二遍）
 *   - System One 的 state 只有这一趟 user 的话与最后一条助手正文
 *   - System One 的答案读不出时判**收工**（判不出来不该把人扣在循环里花钱）
 */
#include <cstdio>
#include <string>

#include "agent/verdict.hpp"

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

static bool has(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

static nlohmann::json text_msg(const char *role, std::string text)
{
    return nlohmann::json{
        {"role", role},
        {"content", nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", std::move(text)}}})}};
}

int main()
{
    printf("== 抄本 ==\n");
    {
        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back(text_msg("user", "改一下 README"));
        msgs.push_back(nlohmann::json{
            {"role", "assistant"},
            {"content", nlohmann::json::array({nlohmann::json{{"type", "thinking"}, {"thinking", "SECRET_THOUGHT"}},
                                               nlohmann::json{{"type", "text"}, {"text", "我看一下"}},
                                               nlohmann::json{{"type", "tool_use"},
                                                              {"id", "t1"},
                                                              {"name", "read"},
                                                              {"input", {{"file_path", "README.md"}}}}})}});
        msgs.push_back(nlohmann::json{
            {"role", "user"},
            {"content", nlohmann::json::array({nlohmann::json{
                            {"type", "tool_result"},
                            {"tool_use_id", "t1"},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "FILE_BODY"}}})}}})}});

        const std::string t = run_transcript(msgs, 0);
        CHECK(has(t, "[user] 改一下 README"), "用户说的话进抄本");
        CHECK(has(t, "[assistant] 我看一下"), "助手正文进抄本");
        CHECK(has(t, "[tool read]") && has(t, "README.md"), "工具调用带名字与参数");
        CHECK(has(t, "[result] FILE_BODY"), "工具结果压成文本进抄本");
        CHECK(!has(t, "SECRET_THOUGHT"), "thinking 不进抄本——判「做到了没有」用不上它");

        // from 是这一趟开跑那一刻的历史长度：判的是这一趟，不是这个会话的一生
        CHECK(!has(run_transcript(msgs, 1), "改一下 README"), "from 之前的历史不进抄本");
    }

    printf("== 裁判自己说过的话 ==\n");
    {
        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back(text_msg("user", "REAL_REQUEST"));
        msgs.push_back(text_msg("user", std::string(kSupervisorTag) + "KICKBACK"));
        const std::string t = run_transcript(msgs, 0);
        CHECK(has(t, "[user] REAL_REQUEST"), "用户的那条标成 user");
        CHECK(has(t, "[supervisor] KICKBACK") && !has(t, "[user] [supervisor]"),
              "打回的那条标成 supervisor，不冒充用户");
    }

    printf("== 超预算怎么裁 ==\n");
    {
        // 一条真用户消息 + 一大堆工具结果。丢的是过程，留的是意图
        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back(text_msg("user", "USER_INTENT"));
        for (int i = 0; i < 200; ++i)
        {
            msgs.push_back(nlohmann::json{
                {"role", "user"},
                {"content", nlohmann::json::array({nlohmann::json{
                                {"type", "tool_result"},
                                {"tool_use_id", "t"},
                                {"content", nlohmann::json::array({{{"type", "text"}, {"text", std::string(1000, 'x')}}})}}})}});
        }
        const std::string t = run_transcript(msgs, 0);
        CHECK(t.size() <= kVerdictCap + 64, "抄本进了预算");
        CHECK(has(t, "USER_INTENT"), "用户消息一条不丢，哪怕它最旧");
        CHECK(has(t, "earlier steps omitted"), "丢过东西就说一声，不静默丢");
        CHECK(has(t, "truncated"), "单条长结果按行截断，也说一声");
    }

    printf("== System One 的 state ==\n");
    {
        nlohmann::json msgs = nlohmann::json::array();
        msgs.push_back(text_msg("user", "BEFORE_RUN"));
        msgs.push_back(text_msg("assistant", "OLD_REPLY"));
        const size_t from = msgs.size();
        msgs.push_back(text_msg("user", "FIRST_ASK"));
        msgs.push_back(nlohmann::json{
            {"role", "assistant"},
            {"content", nlohmann::json::array({nlohmann::json{{"type", "thinking"}, {"thinking", "SECRET_THOUGHT"}},
                                               nlohmann::json{{"type", "text"}, {"text", "MID_REPLY"}},
                                               nlohmann::json{{"type", "tool_use"},
                                                              {"id", "t1"},
                                                              {"name", "read"},
                                                              {"input", {{"file_path", "README.md"}}}}})}});
        msgs.push_back(nlohmann::json{
            {"role", "user"},
            {"content", nlohmann::json::array({nlohmann::json{
                            {"type", "tool_result"},
                            {"tool_use_id", "t1"},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "FILE_BODY"}}})}}})}});
        msgs.push_back(text_msg("user", std::string(kSupervisorTag) + "KICKBACK"));
        msgs.push_back(text_msg("user", "SECOND_ASK"));
        msgs.push_back(text_msg("assistant", "LAST_REPLY"));

        const nlohmann::json st = decision_state(msgs, from);
        CHECK(st.size() == 2, "state 只有 user 与 assistant 两个字段");
        CHECK(st["user"] == "FIRST_ASK\n\nSECOND_ASK", "这一趟 user 的话以空行相接，不含之前的、打回的、工具结果");
        CHECK(st["assistant"] == "LAST_REPLY", "assistant 只取最后一条的正文");

        nlohmann::json one = nlohmann::json::array({text_msg("user", "ASK"),
                                                    nlohmann::json{{"role", "assistant"},
                                                                   {"content", nlohmann::json::array({{{"type", "text"}, {"text", "A"}},
                                                                                                      {{"type", "text"}, {"text", "B"}}})}}});
        CHECK(decision_state(one, 0)["assistant"] == "AB", "最后一条助手消息的多个正文块拼在一起");

        const nlohmann::json big = decision_state(nlohmann::json::array({text_msg("user", std::string(kDecisionFieldCap + 10, 'u')),
                                                                         text_msg("assistant", std::string(kDecisionFieldCap + 10, 'a'))}),
                                                  0);
        CHECK(big["user"] == std::string(kDecisionFieldCap, 'u') + "…(truncated)", "user 超长截断并说明");
        CHECK(big["assistant"] == std::string(kDecisionFieldCap, 'a') + "…(truncated)", "assistant 超长截断并说明");
    }

    printf("== System One 的问题与答案 ==\n");
    {
        const nlohmann::json q = decision_questions();
        CHECK(q["done"]["type"] == "noul", "问的是 noul 类型的 done");
        CHECK(q["done"]["criteria"]["true"].is_string() && q["done"]["criteria"]["false"].is_string(), "true / false 各有判据");

        const auto ans = [](double n) { return nlohmann::json{{"done", {{"type", "noul"}, {"noul", n}}}}; };
        CHECK(decision_done(ans(0.83)), "0.83 → 收工");
        CHECK(decision_done(ans(0.5)), "0.5 → 收工（含边界）");
        CHECK(decision_done(ans(0.2)) == false, "0.2 → 没干完");
        CHECK(decision_done(nlohmann::json::object()), "没有 done → 收工");
        CHECK(decision_done(nlohmann::json{{"done", "yes"}}), "done 不是对象 → 收工");
        CHECK(decision_done(nlohmann::json{{"done", {{"noul", "0.1"}}}}), "noul 不是数字 → 收工");
        CHECK(decision_done(nlohmann::json(nullptr)), "answers 不是对象 → 收工");
    }

    printf("== recap ==\n");
    {
        CHECK(clip_recap("改了 README") == "改了 README", "recap 原文照留");
        CHECK(clip_recap(std::string(kRecapCap + 1, 'r')) == std::string(kRecapCap, 'r') + "…(truncated)", "超长截断并说明");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
