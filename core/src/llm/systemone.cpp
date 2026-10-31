/*
 * systemone.cpp — System One 的请求与响应（见 systemone.hpp）
 */
#include "llm/systemone.hpp"

namespace realagent {

HttpRequest systemone_request(const Config &cfg, const std::string &model, const nlohmann::json &state,
                              const nlohmann::json &questions)
{
    return {.url = cfg.get("base_url") + "/v1/systemone",
            .headers = {"Authorization: Bearer " + cfg.get("api_key"), "Content-Type: application/json"},
            .body = nlohmann::json{{"model", model}, {"state", state}, {"questions", questions}}.dump()};
}

std::expected<nlohmann::json, std::string> systemone_answers(const std::string &body)
{
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (!j.is_object() || !j.value("answers", nlohmann::json()).is_object())
        return std::unexpected("System One 的响应里没有 answers 对象：" + body.substr(0, 200));
    return j;
}

} // namespace realagent
