/*
 * verdict.hpp — 收工判定（ADR-0025）：拼抄本、拼 prompt、解回答，全是纯函数
 *
 * 主模型一轮没调工具时，把这一趟的抄本交给小模型，它回 recap 与 done。
 * recap 排在 done 前面：先把干过的事数一遍，再判够不够。
 * 抄本裁法：user 消息一条不丢（那是意图），其余按行截断，超预算从旧到新丢。
 * 真正发请求的是 Agent::judge()。
 */
#pragma once

#include <cstddef>
#include <string>

#include <realetting/json.hpp>

namespace realagent {

/* 解不出来时 done 为真：判不出来不该把人扣在循环里。 */
struct StopVerdict {
    bool done = true;
    std::string reason; // 打回时告诉主模型还差什么
    std::string recap;  // 这一趟干了什么（完成通知与 agent_end 帧带它）
};

/* 打回消息的前缀。它进历史也是 user 消息，下一次判定要认得出这是裁判自己说的。 */
inline constexpr const char *kSupervisorTag = "[supervisor] ";

inline constexpr size_t kVerdictLineCap = 600; // 一行截到这么长
inline constexpr size_t kVerdictCap = 24000;   // 整份抄本的预算

/* messages[from..] → 抄本。from 是这一趟开跑时的历史长度。 */
std::string run_transcript(const nlohmann::json &messages, size_t from);

const char *stop_verdict_system();
std::string stop_verdict_prompt(const std::string &transcript);

/* 回的不是合法 JSON 时：原文当 recap、判收工，不重试。 */
StopVerdict parse_stop_verdict(const std::string &text);

} // namespace realagent
