/*
 * test_verdict.cpp — 收工判定的三个纯函数（ADR-0025）
 *
 * 这里不打 LLM 调用，也不起 agent：judge() 要的是网络与配置，而会坏的不是那一次
 * curl，是这三件事——
 *   - 抄本里 user 消息一条不丢（那是意图，丢了裁判就没法判「用户要的做到了没有」）
 *   - 抄本里认得出裁判自己上次说的话（不标的话它会读成用户的新要求，然后催第二遍）
 *   - 小模型回来的东西解不出时判**收工**（判不出来不该把人扣在循环里花钱）
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

    printf("== 解回答 ==\n");
    {
        const StopVerdict a = parse_stop_verdict(R"({"recap":"改了 README","done":true,"reason":""})");
        CHECK(a.done && a.recap == "改了 README", "正常的 JSON");

        const StopVerdict b = parse_stop_verdict("好的，我看了一下：\n{\"recap\":\"R\",\"done\":false,\"reason\":\"测试没跑\"}\n");
        CHECK(!b.done && b.reason == "测试没跑", "前后带闲话也认——只取第一个 { 到最后一个 }");

        const StopVerdict c = parse_stop_verdict(R"({"recap":"R","done":"false","reason":"还差一步"})");
        CHECK(!c.done, "done 写成字符串也认——小模型常这么干");

        const StopVerdict d = parse_stop_verdict("这一趟把 README 改好了。");
        CHECK(d.done && has(d.recap, "README"), "回的不是 JSON：那段话当 recap，判收工");

        const StopVerdict e = parse_stop_verdict("");
        CHECK(e.done, "什么都没回也判收工——判不出来不该把人扣在循环里花钱");

        const StopVerdict f = parse_stop_verdict(R"({"recap":"R"})");
        CHECK(f.done, "没写 done 就是收工，不是「没说所以接着干」");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    return failures ? 1 : 0;
}
