/*
 * compact.hpp — 压缩（ADR-0036）：把历史换成一条带标记的摘要。这里全是纯函数
 *
 * 摘要是一条 user 消息，正文以 kCompactTag 开头。它照常追加进会话文件，读回时从最后
 * 一条摘要读起——之前的行留在盘上给回放看，不再发给模型。
 * 真正发请求的是 Agent::compact()。
 */
#pragma once

#include <cstddef>
#include <string>

#include "json.hpp"

namespace realagent {

/* 摘要消息的前缀。与 kSupervisorTag 同一个做法：靠正文认来源，不往消息里加键（会原样发给端点）。 */
inline constexpr const char *kCompactTag = "[compact] ";

/* 抄本的预算（字节）。约 25k–30k token，塞得进任何还在用的窗口——溢出时也发得出去。 */
inline constexpr size_t kCompactTranscriptCap = 100000;

/* 文本是不是一条摘要（kCompactTag 开头）。 */
bool is_compact_text(const std::string &text);

/* messages 里最后一条摘要的下标；没有就是 0。读回历史从这里开始。 */
size_t compact_begin(const nlohmann::json &messages);

/* 摘要 → 要入账的那条 user 消息。 */
nlohmann::json compact_message(const std::string &summary);

/* 写摘要那次请求：system 与带抄本的 user 消息。instructions = /compact 的参数与 PreCompact 注入，可空。 */
const char *compact_system();
std::string compact_prompt(const std::string &transcript, const std::string &instructions);

} // namespace realagent
