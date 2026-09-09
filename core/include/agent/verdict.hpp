/*
 * verdict.hpp — 收工判定与 recap：小模型在出口上的那一次判断（ADR-0025）
 *
 * agent loop 的出口从前是一个工具：主模型自己调 `stop`。那是让干活的那个模型
 * 兼任裁判——它对「结束」的意识本来就不清晰，于是要在 system prompt 里写契约、
 * 配范例，还要在它忘了的时候补一条提醒，跑一圈再问一次。三处补丁，一个病根。
 *
 * 现在出口是一次判断：**主模型这一轮没有下一步动作（没调工具）时，把这一趟的抄本
 * 交给小模型**，它回一份 StopVerdict——收工就收工，没干完就打回让主模型接着干。
 * 主模型的工具表里因此没有 `stop`，它也不需要知道有裁判这回事。
 *
 * 一次调用两件事，因为它们要的输入是同一份：
 *   - **recap**：这一趟干了什么，写给没看过过程的人（也写给邻居 agent 的完成通知）
 *   - **done**：干完了没有
 * recap 排在 done 前面不是排版：模型先把干过的事数一遍，再判断数完的这些够不够。
 *
 * 抄本怎么裁，抄的是 Claude Code 压缩上下文那套：**user 消息一条不丢**（那是意图，
 * 丢了就无从判断「用户要的做到了没有」），助手正文与工具结果按行截断，超了预算从旧
 * 到新丢——丢掉的是过程，留下的是意图与现场。
 *
 * 这里只有纯函数：拼抄本、拼 prompt、解回答。真正打那次 LLM 调用的是 Agent::judge()，
 * 它才认识 curl 与配置。
 */
#pragma once

#include <cstddef>
#include <string>

#include "json.hpp"

namespace realagent {

/* 小模型在出口上的裁决。**跟 approval.hpp 那个 Verdict 不是一回事**：那个判
 * 一次工具调用放不放行，这个判一趟活干完没有。
 *
 * **解不出来时 done 为真**：判定失败不该把人扣在循环里
 * 花钱，退化回「没调工具就是干完了」正是这个功能之前的行为。 */
struct StopVerdict {
    bool done = true;
    std::string reason; // 打回时告诉主模型还差什么；done 时为空
    std::string recap;  // 这一趟的回顾（完成通知与 agent_end 帧带的就是它）
};

/* 打回时那条消息的开头。它记进历史时也是一条 user 消息（凡是从 agent 外面来的输入
 * 都是 user），下一次判定就会在抄本里再看见它。**认出来标成 supervisor**：
 * 不标的话裁判会把自己上一次说的话读成用户的新要求，然后照着它去催第二遍。 */
inline constexpr const char *kSupervisorTag = "[supervisor] ";

/* 一行截到这么长。工具结果动辄几十 KB，判定要的是「跑过什么、成没成」，不是全文。 */
inline constexpr size_t kVerdictLineCap = 600;
/* 整份抄本的预算。超了从旧到新丢非 user 的行。 */
inline constexpr size_t kVerdictCap = 24000;

/* messages[from..] → 一份给小模型看的抄本。from 是这一趟开跑那一刻的历史长度：
 * 判的是这一趟，不是这个会话的一生。 */
std::string run_transcript(const nlohmann::json &messages, size_t from);

/* 小模型的 system prompt（裁判的职责 + recap 的写法 + 回什么形状） */
const char *stop_verdict_system();

/* 小模型的 user prompt：抄本包一层 */
std::string stop_verdict_prompt(const std::string &transcript);

/* 小模型回的那段文字 → StopVerdict。**它不是非得回合法 JSON**——回不出来时把它那段话
 * 原样当 recap、判收工，不报错、不重试：这是杂活模型，为它加一层重试只是把一次
 * 判不出来变成三次判不出来。 */
StopVerdict parse_stop_verdict(const std::string &text);

} // namespace realagent
