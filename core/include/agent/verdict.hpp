/*
 * verdict.hpp — 收工判定（ADR-0032）：拼 System One 的输入、读它的答案、拼 recap 的 prompt，全是纯函数
 *
 * 主模型一轮没调工具时，decision_model（System One，仅 revlm）判 done；干完了才让小模型
 * 拿这一趟的抄本写 recap。抄本裁法：user 消息一条不丢（那是意图），其余按行截断，超预算从旧到新丢。
 * 真正发请求的是 Agent::decide() 与 Agent::recap()。
 */
#pragma once

#include <cstddef>
#include <string>

#include "json.hpp"

namespace realagent {

/* 打回消息的前缀。它进历史也是 user 消息，下一次判定要认得出这是裁判自己说的。 */
inline constexpr const char *kSupervisorTag = "[supervisor] ";

/* 打回消息。System One 只回 done 的把握，不说差什么，所以是固定的一句 */
inline constexpr const char *kKickback =
    "[supervisor] This run is not finished yet. Keep working on it. Do not start tasks the user did not request.";

inline constexpr size_t kVerdictLineCap = 600;    // 一行截到这么长
inline constexpr size_t kVerdictCap = 24000;      // 整份抄本的预算
inline constexpr size_t kDecisionFieldCap = 8000; // System One 的 state 每个字段截到这么长
inline constexpr size_t kRecapCap = 4000;
/* System One 那次调用的超时：没人看着它，卡住就永远收不了工 */
inline constexpr long kDecisionTimeoutS = 30;

/* messages[from..] → 抄本。from 是这一趟开跑时的历史长度。 */
std::string run_transcript(const nlohmann::json &messages, size_t from);

/* System One 的 state：{user, assistant}。user = 这一趟里 user 的话（messages[from..]，
 * 不含 supervisor 打回的），assistant = 最后一条助手消息的正文；别的一概不给。 */
nlohmann::json decision_state(const nlohmann::json &messages, size_t from);

/* System One 的问题：{done: ...} */
nlohmann::json decision_questions();

/* answers.done.noul >= 0.5 为收工；缺了或形状不对也按收工。 */
bool decision_done(const nlohmann::json &answers);

const char *recap_system();
std::string recap_prompt(const std::string &transcript);

/* 小模型回的 recap 原文，超长截断。 */
std::string clip_recap(std::string text);

} // namespace realagent
