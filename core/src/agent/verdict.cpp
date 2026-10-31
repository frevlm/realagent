/*
 * verdict.cpp — System One 的输入与答案、抄本、recap 的 prompt。纯函数，不碰网络也不碰配置。
 */
#include "agent/verdict.hpp"

#include "tools/tools.hpp"

#include <vector>

namespace realagent {

namespace {

/* 超长就截断并说明截了。**不静默截**：模型（和读日志的人）知道后面还有东西，
 * 就不会把「没看见」当成「没有」。 */
std::string clip(std::string s, size_t cap)
{
    if (s.size() <= cap) return s;
    s.resize(cap);
    s += "…(truncated)";
    return s;
}

/* 抄本里的一行。pinned = 用户说的话，超预算也不丢——那是意图，丢了就没法判
 * 「用户要的做到了没有」。 */
struct Line {
    bool pinned = false;
    std::string text;
};

} // namespace

std::string run_transcript(const nlohmann::json &messages, size_t from)
{
    std::vector<Line> lines;
    if (messages.is_array())
    {
        for (size_t i = from; i < messages.size(); ++i)
        {
            const nlohmann::json &m = messages[i];
            const std::string role = m.value("role", std::string());
            const auto c = m.find("content");
            if (c == m.end() || !c->is_array()) continue;
            for (const nlohmann::json &b : *c)
            {
                const std::string t = b.value("type", std::string());
                if (t == "text" && role == "user")
                {
                    const std::string text = b.value("text", std::string());
                    const bool mine = text.rfind(kSupervisorTag, 0) == 0;
                    lines.push_back({!mine, (mine ? "" : "[user] ") + clip(text, kVerdictLineCap)});
                }
                else if (t == "text")
                {
                    lines.push_back({false, "[assistant] " + clip(b.value("text", std::string()), kVerdictLineCap)});
                }
                else if (t == "tool_use")
                {
                    lines.push_back({false, "[tool " + b.value("name", std::string()) + "] " +
                                                clip(b.value("input", nlohmann::json::object()).dump(),
                                                     kVerdictLineCap)});
                }
                else if (t == "tool_result")
                {
                    lines.push_back({false,
                                     std::string(b.value("is_error", false) ? "[result FAILED] " : "[result] ") +
                                         clip(tool_content_text(b.value("content", nlohmann::json::array())),
                                              kVerdictLineCap)});
                }
                /* thinking 不进抄本：那是模型对自己说的话，判「做到了没有」用不上，
                 * 而它常常是整段历史里最长的一块。 */
            }
        }
    }

    size_t total = 0;
    for (const Line &l : lines) total += l.text.size() + 1;

    // 超预算就从最旧的开始丢，只丢不 pinned 的。丢掉的是过程，留下的是意图与现场
    std::vector<char> dropped(lines.size(), 0);
    for (size_t i = 0; i < lines.size() && total > kVerdictCap; ++i)
    {
        if (lines[i].pinned) continue;
        dropped[i] = 1;
        total -= lines[i].text.size() + 1;
    }

    std::string out;
    bool marked = false; // 丢过的说一声，连着丢一串只说一次
    for (size_t i = 0; i < lines.size(); ++i)
    {
        if (dropped[i])
        {
            if (!marked) out += "…(earlier steps omitted)\n";
            marked = true;
            continue;
        }
        marked = false;
        out += lines[i].text;
        out += '\n';
    }
    // 全是 user 的话仍旧可能超（一趟里塞进几十条长消息）。这时整份再截一刀
    return clip(std::move(out), kVerdictCap);
}

nlohmann::json decision_state(const nlohmann::json &messages, size_t from)
{
    std::string user, assistant;
    if (messages.is_array())
    {
        for (size_t i = from; i < messages.size(); ++i)
        {
            const std::string role = messages[i].value("role", std::string());
            const auto c = messages[i].find("content");
            if (c == messages[i].end() || !c->is_array() || (role != "user" && role != "assistant")) continue;
            if (role == "assistant") assistant.clear(); // 只留最后一条
            for (const nlohmann::json &b : *c)
            {
                if (b.value("type", std::string()) != "text") continue;
                const std::string text = b.value("text", std::string());
                if (role == "assistant")
                    assistant += text;
                else if (text.rfind(kSupervisorTag, 0) != 0)
                    user += (user.empty() ? "" : "\n\n") + text;
            }
        }
    }
    return {{"user", clip(std::move(user), kDecisionFieldCap)},
            {"assistant", clip(std::move(assistant), kDecisionFieldCap)}};
}

nlohmann::json decision_questions()
{
    return {{"done",
             {{"type", "noul"},
              {"instructions", "Has the assistant finished what the user asked?"},
              {"criteria",
               {{"true", "every explicit user request is carried out, or the reply asks the user something it cannot proceed without"},
                {"false", "an explicit user request is left unfinished, or the reply promises work it did not do"}}}}}};
}

bool decision_done(const nlohmann::json &answers)
{
    const nlohmann::json noul = answers.is_object() ? answers.value("/done/noul"_json_pointer, nlohmann::json()) : nlohmann::json();
    return !noul.is_number() || noul.get<double>() >= 0.5;
}

const char *recap_system()
{
    return "You write the recap of a run of an autonomous coding agent, for someone who did not "
           "watch it. In plain prose, under 200 words, covering only what the transcript shows:\n"
           "1. What the user asked for, and what they were after.\n"
           "2. What was done: files created or changed, commands run, decisions made.\n"
           "3. Errors hit, and how they were resolved.\n"
           "4. Anything still unfinished.\n"
           "5. The state things are left in.\n"
           "Never invent work that is not in the transcript. Reply with the recap and nothing else.";
}

std::string recap_prompt(const std::string &transcript)
{
    return "<transcript>\n" + transcript + "</transcript>\n\nWrite the recap now.";
}

std::string clip_recap(std::string text) { return clip(std::move(text), kRecapCap); }

} // namespace realagent
