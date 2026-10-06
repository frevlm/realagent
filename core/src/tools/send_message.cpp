/*
 * send_message.cpp — send_message 工具的定义。实现在 Executor（它要认识 Agents）。
 */
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json send_message_def()
{
    return tool_def(
        "send_message", "给别的 agent 发消息",
        "Deliver a message into another agent's inbox. You may only send to the numeric ids\n"
        "of agents you have an out edge to — without an edge you do not know it exists.",
        R"({"type":"object","properties":{"to":{"type":"integer"},"text":{"type":"string"}},"required":["to","text"]})",
        false);
}

} // namespace realagent
