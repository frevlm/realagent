/*
 * config.hpp — 配置：代码里的默认树打底，~/.realagent/settings.json 覆盖（ADR-0010）
 *
 * 文件读写交给 realetting（ADR-0027）：文件不存在就建一个 {}，默认值只在内存里，
 * persist 只把改动写进文件。不热重载。settings.json 不是合法 JSON 对象时 load 失败。
 * 端点那一束（protocol / base_url / model）没有默认值，理由见 llm.hpp。
 * 两档模型（model / small_model）共用 base_url 与 api_key，档位间不回落。
 */
#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "realetting.hpp"

namespace realagent {

enum class ModelTier {
    Main, // 对话主链路
    Small // 杂活（收工判定）
};

class Config {
  public:
    static std::expected<Config, std::string> load();

    std::string get(std::string_view key) const;         // 未知键、非字符串返回空串
    nlohmann::json get_json(std::string_view key) const; // 非字符串配置项用；未知键返回 null

    std::string model(ModelTier tier) const;

    /* 写回 settings.json 这一个键。文件是坏 JSON 时拒绝写入：不能盖掉读不懂的用户数据。 */
    bool persist(std::string_view key, const nlohmann::json &v);

    /* ~/.realagent/models.json：存在即整表替换出厂模型表（ADR-0009）。 */
    std::string models_path() const;

  private:
    explicit Config(realetting::Ref settings) : settings_(std::move(settings)) {}

    realetting::Ref settings_;
};

std::string getenv_or(std::string_view name, std::string_view fallback = "");

} // namespace realagent
