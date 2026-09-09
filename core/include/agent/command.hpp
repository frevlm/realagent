/*
 * command.hpp — 斜杠命令（/new、/resume、/model）
 *
 * 两个门通向这里：`POST /message` 的 `/` 前缀、`POST /command`。一份实现——
 * 两份迟早只改一边，那时同一条命令在两个端点上行为不同，谁都查不出为什么。
 * 命令不投收件箱，直接返回结果。
 *
 * 命令集只有一张表：`GET /commands` 列的是它，派发认的也是它，
 * 不存在"两处同步"这个义务。表里两类（ADR-0024 §7）——
 *
 *   builtin  core 的一个动作（command.cpp 的 kCommands）。拿锁、直接返回结果
 *   prompt   plugin 带来的一段文字。展开后投收件箱、**不拿锁**
 *
 * 派发因此多**一个**分支，判的是「core 的一个动作，还是一段要发出去的文字」——
 * 一个真区别，同 Executor::execute 为 MCP 多的那一个。
 *
 * 那个 try_lock 在 builtin 那一支里面，不在函数开头：prompt 命令等价于用户打了一段字，
 * 而 POST /message 本来就不拿锁。让它回 AGENT_BUSY 是把发消息变得比原来更难。
 */
#pragma once

#include <string>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/context.hpp"
#include "json.hpp"

namespace realagent {

/* 事件循环线程拿不到 agent 的锁时回的那句话。
 *
 * 这些回调跑在事件循环那唯一一条线程上（quic_server.cpp 的 poll 循环）。
 * 在那儿阻塞等锁，等的是"一次 run 跑完"——期间收不了任何请求，
 * 包括那个唯一能把 run 停下来的 POST /interrupt，也推不出任何事件帧。
 * 用户打一条 /new 想放弃当前任务，换来的是整个客户端假死到 run 自己结束。
 *
 * 所以宁可当场说"忙着呢"：拿不到锁就回一句人话，让他先中断。
 * 一次拒绝是一句话，一次假死是没有话。 */
inline constexpr const char *AGENT_BUSY =
    "agent 正在运行——先中断（Esc / POST /interrupt）再执行这条命令";

/* 失败载荷：{"ok":false,"error":msg} */
std::string command_error(const std::string &msg);

/* 内置那三条的名字。prompt 命令撞上就跳过——覆盖掉 `/new` 就没法开新会话
 * （同内置六个工具的理由）。 */
bool is_builtin_command(const std::string &name);

/* 斜杠命令清单（GET /commands，TUI 菜单数据源）。core 是唯一真相源。
 *
 * **agent 可以为空**：prompt 命令表跟着 agent 的 workdir 走，没指名道姓时只回内置那三条。
 * 这不是降级，是那个问题在没有 agent 时没有答案（同 ADR-0022 砍掉项目级 settings 的判据）。 */
nlohmann::json command_defs(const Agent *agent = nullptr);

/* 会话清单（GET /sessions、/new、/resume 共用）：盘上有哪些会话，以及每一个被谁打开着。
 *
 * **`current: bool` 换成 `opened_by`（ADR-0019 §10）**：多 agent 之后「当前」没有主语了，
 * 同一个目录下可以有 N 个 agent 各自打开着一个会话。一个会话要么被某个 agent 打开着，
 * 要么躺在盘上——`opened_by` 直接说的就是这句话，不需要客户端再去问「谁的当前」。 */
nlohmann::json sessions_payload(const Agents &pool, const Agent &agent);

/* 状态栏载荷：配的模型名 + 数据表里查到的元数据（GET /statusline、statusline 帧）。 */
nlohmann::json statusline_payload(const CoreContext &ctx);

/* 执行一条斜杠命令（input 带前导 `/`），返回响应 JSON 字符串。
 * agent 的锁在这里面拿：拿不到就回 AGENT_BUSY，不排队等。 */
std::string handle_command(CoreContext &ctx, Agents &pool, Agent &agent,
                           const std::string &input);

} // namespace realagent
