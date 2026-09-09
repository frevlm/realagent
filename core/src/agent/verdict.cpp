/*
 * verdict.cpp — 抄本、prompt、解回答。三个纯函数，不碰网络也不碰配置。
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

bool truthy(const nlohmann::json &v, bool fallback)
{
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    // 小模型把 JSON 的 true 写成字符串是常事，认它
    if (v.is_string())
    {
        const std::string s = v.get<std::string>();
        if (s == "true" || s == "yes") return true;
        if (s == "false" || s == "no") return false;
    }
    return fallback;
}

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

const char *stop_verdict_system()
{
    return "You supervise an autonomous coding agent. The agent has just replied without calling any "
           "tools, which means it believes it has nothing left to do. You decide whether its run is "
           "really finished, and you write the recap of that run.\n"
           "\n"
           "Reply with one JSON object and nothing else:\n"
           "{\"recap\": \"...\", \"done\": true, \"reason\": \"...\"}\n"
           "\n"
           "Write \"recap\" first, then judge — count what was actually done before deciding whether "
           "it is enough. The recap is a factual summary of THIS run, written for someone who did not "
           "watch it. In plain prose, under 200 words, covering only what the transcript shows:\n"
           "1. What the user asked for, and what they were after.\n"
           "2. What was done: files created or changed, commands run, decisions made.\n"
           "3. Errors hit, and how they were resolved.\n"
           "4. Anything still unfinished.\n"
           "5. The state things are left in.\n"
           "Never invent work that is not in the transcript.\n"
           "\n"
           "\"done\" is true when every explicit request from the user has been carried out, and also "
           "when the agent's last message asks the user a question it cannot proceed without.\n"
           "\"done\" is false only when the transcript shows an explicit user request left unfinished, "
           "or work the agent itself said it would do next and then did not do. Extra polishing, "
           "testing, refactoring or documentation that nobody asked for is never a reason to keep "
           "going — do not invent tasks the user did not request.\n"
           "\"reason\" names the unfinished thing in one sentence; leave it empty when done is true.";
}

std::string stop_verdict_prompt(const std::string &transcript)
{
    return "<transcript>\n" + transcript + "</transcript>\n\nReply with the JSON object now.";
}

StopVerdict parse_stop_verdict(const std::string &text)
{
    StopVerdict v;
    const size_t b = text.find('{'), e = text.rfind('}');
    const nlohmann::json j = (b == std::string::npos || e == std::string::npos || e < b)
                                 ? nlohmann::json(nullptr)
                                 : nlohmann::json::parse(text.substr(b, e - b + 1), nullptr, false);
    if (!j.is_object())
    {
        // 回的不是 JSON：它那段话就是 recap，判收工。判不出来不该把人扣在循环里花钱
        v.recap = clip(text, kVerdictLineCap * 2);
        return v;
    }
    v.recap = j.value("recap", std::string());
    v.done = truthy(j.value("done", nlohmann::json(true)), true);
    if (!v.done) v.reason = j.value("reason", std::string());
    return v;
}

} // namespace realagent
