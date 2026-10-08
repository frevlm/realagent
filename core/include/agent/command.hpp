/*
 * command.hpp — 斜杠命令。POST /message 的 `/` 前缀与 POST /command 走同一份实现。
 *
 * 两类：builtin 是 core 的动作，直接返回结果；prompt 是 plugin 带来的一段文字，
 * 展开后当消息发出去，等价于用户打了一段字。
 * /new 与 /resume 不在这里：对话是客户端选的，换一段对话不需要 core 做任何事（ADR-0029）。
 */
#pragma once

#include <string>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/context.hpp"
#include "json.hpp"

namespace realagent {

/* {"ok":false,"error":msg} */
std::string command_error(const std::string &msg);

/* 内置命令与客户端就地处理的命令占着的名字：plugin 的命令不许撞上。 */
bool is_builtin_command(const std::string &name);

/* GET /commands：内置的，加上 workdir 那里 plugin 带来的。 */
nlohmann::json command_defs(const std::string &workdir);

/* 对话清单：workdir 下盘上有哪些对话，哪段正在跑、哪段在别的窗口里开着。 */
nlohmann::json sessions_payload(const Agents &pool, const std::string &client,
                                const std::string &workdir);

/* 模型名 + 模型表里查到的元数据（GET /statusline、statusline 帧）。 */
nlohmann::json statusline_payload(const CoreContext &ctx);

/* reply 非空：原样回给客户端。否则 post 是展开好的正文，要当一条消息发出去。 */
struct CommandOutcome {
    std::string reply;
    std::string post;
};

/* input 带前导 `/`。agent 是这段对话打开着的那个，没有就空（/plugins 拿它报连接错误）。 */
CommandOutcome run_command(CoreContext &ctx, const std::string &workdir, const Agent *agent,
                           const std::string &input);

} // namespace realagent
