/*
 * command.hpp — 斜杠命令。POST /message 的 `/` 前缀与 POST /command 走同一份实现。
 *
 * 一张表两类：builtin 是 core 的动作（拿锁、直接返回结果）；prompt 是 plugin 带来的
 * 一段文字（展开后投收件箱、不拿锁，等价于用户打了一段字）。
 */
#pragma once

#include <string>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/context.hpp"
#include <realetting/json.hpp>

namespace realagent {

/* 事件循环线程拿不到 agent 的锁时回这句。在那条线程上等锁会让整个客户端假死，
 * 连 POST /interrupt 都收不到。 */
inline constexpr const char *AGENT_BUSY =
    "agent 正在运行——先中断（Esc / POST /interrupt）再执行这条命令";

/* {"ok":false,"error":msg} */
std::string command_error(const std::string &msg);

bool is_builtin_command(const std::string &name);

/* GET /commands。agent 为空时只有内置命令（prompt 命令跟着 workdir 走）。 */
nlohmann::json command_defs(const Agent *agent = nullptr);

/* 会话清单：盘上有哪些会话，每一个被哪个 agent 打开着（opened_by）。 */
nlohmann::json sessions_payload(const Agents &pool, const Agent &agent);

/* 模型名 + 模型表里查到的元数据（GET /statusline、statusline 帧）。 */
nlohmann::json statusline_payload(const CoreContext &ctx);

/* input 带前导 `/`。返回响应 JSON 文本。 */
std::string handle_command(CoreContext &ctx, Agents &pool, Agent &agent,
                           const std::string &input);

} // namespace realagent
