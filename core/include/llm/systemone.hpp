/*
 * systemone.hpp — revlm 的 System One：POST /v1/systemone，一问一答，没有 SSE（ADR-0032）
 *
 * 它不是聊天协议，不进 Protocol / SseParser / build_request。调用方用 send_revlm 发。
 */
#pragma once

#include <expected>
#include <string>

#include "json.hpp"
#include "llm/llm.hpp"

namespace realagent {

/* state / questions 原样放进体里；不带 stream。 */
HttpRequest systemone_request(const Config &cfg, const std::string &model, const nlohmann::json &state,
                              const nlohmann::json &questions);

/* 响应体 → 根对象（要有 answers 对象，usage 等字段随根一起交回）；否则返回人话。 */
std::expected<nlohmann::json, std::string> systemone_answers(const std::string &body);

} // namespace realagent
