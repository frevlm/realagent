/*
 * config.hpp — 配置：代码里的默认树打底，~/.realagent/settings.json 逐键覆盖（ADR-0010）
 *
 * 启动读一次，不热重载。只有 settings.json 存在但不是合法 JSON 时 load 失败。
 * 端点那一束（protocol / base_url / model）没有默认值，理由见 llm.hpp。
 * 两档模型（model / small_model）共用 base_url 与 api_key，档位间不回落。
 * 线程安全：内部 mutex 护配置树。
 */
#pragma once

#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "json.hpp"

namespace realagent {

enum class ModelTier {
    Main, // 对话主链路
    Small // 杂活（收工判定）
};

class Config {
  public:
    static std::expected<Config, std::string> load();

    std::string get(std::string_view key) const;         // 未知键返回空串
    nlohmann::json get_json(std::string_view key) const; // 非字符串配置项用；未知键返回 null
    bool has(std::string_view key) const;

    std::string model(ModelTier tier) const;

    /* 只改 settings.json 里这一个键（tmp+rename），成功后再改内存。默认值不会渗进文件。
     * 文件是坏 JSON 时拒绝写入：不能盖掉读不懂的用户数据。 */
    bool persist(std::string_view key, const nlohmann::json &v);

    /* ~/.realagent/models.json：存在即整表替换出厂模型表（ADR-0009）。 */
    std::string models_path() const;

    nlohmann::json to_json() const;

  private:
    nlohmann::json settings_;
    // mutex 不可拷贝，包一层让 Config 能按值返回
    mutable std::shared_ptr<std::mutex> mutex_ = std::make_shared<std::mutex>();
};

std::string getenv_or(std::string_view name, std::string_view fallback = "");

} // namespace realagent
