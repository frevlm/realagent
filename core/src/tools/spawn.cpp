/*
 * spawn.cpp — spawn 工具的定义。实现在 Executor（它要认识 Agents）。
 */
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json spawn_def()
{
    return tool_def(
        "spawn", "派生 agent",
        "Launch a sub-agent to handle a self-contained task, and wait for it to finish.\n"
        "The sub-agent starts with a fresh context: it sees only the prompt you give it, not this\n"
        "conversation, so the prompt must carry every detail it needs and say exactly what to\n"
        "report back. Its final message is returned as this tool's result; the user does not see it,\n"
        "so relay whatever matters. Sub-agents cannot spawn agents themselves.\n"
        "Use it to keep long searches or bulky investigations out of your own context.\n"
        "agent: optional. The name of one of the agent definitions listed in your system prompt.\n"
        "Its role description is appended to the sub-agent's system prompt.",
        R"({"type":"object","properties":{"prompt":{"type":"string","description":"the complete task for the sub-agent"},"agent":{"type":"string","description":"optional: name of an agent definition listed in your system prompt"},"workdir":{"type":"string","description":"optional: working directory for the sub-agent; defaults to yours"}},"required":["prompt"]})",
        true);
}

} // namespace realagent
