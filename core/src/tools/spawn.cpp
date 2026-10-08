/*
 * spawn.cpp — spawn 工具的定义。实现在 Executor（它要认识 Agents）。
 */
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json spawn_def()
{
    return tool_def(
        "spawn", "派生 agent",
        "Spawn a new agent in the background. Returns the new agent's numeric id immediately\n"
        "(followed by its session id, which you can ignore) without waiting for it to finish.\n"
        "Agent ids are positive integers incrementing from 1 (1, 2, 3, ...).\n"
        "in_edges: agent ids permitted to send messages to the new agent and receive its\n"
        "completion notice.\n"
        "out_edges: agent ids the new agent is permitted to send messages to.\n"
        "To receive its completion notice, include your own agent id in in_edges.\n"
        "Ids present in both lists establish bidirectional communication.\n"
        "Both lists accept only your own agent id or known agent ids.\n"
        "agent: optional. The name of one of the agent definitions listed in your system prompt.\n"
        "Its role description is appended to the new agent's system prompt.",
        R"({"type":"object","properties":{"workdir":{"type":"string","description":"working directory for the new agent, required"},"prompt":{"type":"string","description":"initial message handed to the new agent"},"in_edges":{"type":"array","items":{"type":"integer"},"description":"agent ids permitted to send messages to this agent and receive its completion notice"},"out_edges":{"type":"array","items":{"type":"integer"},"description":"agent ids this agent is permitted to send messages to"},"agent":{"type":"string","description":"optional: name of an agent definition listed in your system prompt"}},"required":["workdir","prompt"]})",
        true);
}

} // namespace realagent
