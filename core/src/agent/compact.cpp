/*
 * compact.cpp — 摘要消息的认法与写摘要的 prompt。纯函数，不碰网络也不碰配置。
 */
#include "agent/compact.hpp"

namespace realagent {

bool is_compact_text(const std::string &text) { return text.starts_with(kCompactTag); }

size_t compact_begin(const nlohmann::json &messages)
{
    for (size_t i = messages.size(); i-- > 0;)
    {
        const nlohmann::json &m = messages[i];
        if (!m.is_object() || m.value("role", std::string()) != "user") continue;
        const auto c = m.find("content");
        if (c == m.end() || !c->is_array() || c->empty() || !c->front().is_object()) continue;
        if (is_compact_text(c->front().value("text", std::string()))) return i;
    }
    return 0;
}

nlohmann::json compact_message(const std::string &summary)
{
    return {{"role", "user"},
            {"content", nlohmann::json::array({{{"type", "text"}, {"text", kCompactTag + summary}}})}};
}

const char *compact_system()
{
    return "You summarize the transcript of an autonomous coding agent's conversation, so that the agent "
           "can continue the work from your summary alone.";
}

/* 第 1 段排第一：一趟中途压缩后，收工判定从摘要里读「用户要什么」，截断时它最后才被截到。 */
std::string compact_prompt(const std::string &transcript, const std::string &instructions)
{
    std::string p = "<transcript>\n" + transcript +
                    "</transcript>\n\n"
                    "Summarize this conversation. The summary replaces it: the agent will see nothing else of it. "
                    "Use these sections:\n"
                    "1. User requests: every request the user made, quoted verbatim, oldest first.\n"
                    "2. Goal and constraints: what the user is after, and every preference or restriction they stated.\n"
                    "3. Progress: what is done, what is in progress, what is blocked and why.\n"
                    "4. Key decisions: what was decided, and why.\n"
                    "5. Files: every file read, created or changed, with exact paths and what matters about each.\n"
                    "6. Next steps: exactly what to do next.\n"
                    "Keep exact identifiers, paths, commands and error messages. Reply with the summary and nothing else.";
    if (!instructions.empty()) p += "\n\nAdditional instructions for this summary:\n" + instructions;
    return p;
}

} // namespace realagent
