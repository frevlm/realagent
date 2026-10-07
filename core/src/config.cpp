#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace realagent {
namespace fs = std::filesystem;

namespace {

// 默认配置树（ADR-0010）。没列出的键缺省为空串。
nlohmann::json defaults()
{
    // permission：ask（问用户）/ allow-all / deny。
    // mcp_http_bridge：http 型 MCP server 的桥接命令模板（ADR-0024 §5），空数组 = 不支持 http。
    // 占位符 {url} / {name} / {value}；子数组是每个 header 重复一次的那一组。
    return {{"permission", "ask"},
            {"mcp_http_bridge",
             {"npx", "-y", "mcp-remote", "{url}",
              nlohmann::json::array({"--header", "{name}: {value}"})}}};
}

// 全局配置落点：~/.realagent/——唯一的覆盖来源，不看 cwd
fs::path global_dir() { return fs::path(getenv_or("HOME", ".")) / ".realagent"; }

} // namespace

std::string getenv_or(std::string_view name, std::string_view fallback)
{
    if (const char *v = std::getenv(std::string(name).c_str()); v != nullptr)
        return std::string(v);
    return std::string(fallback);
}

std::expected<Config, std::string> Config::load()
{
    try
    {
        realetting::Dir dir(global_dir());
        return Config(dir.open("settings.json", defaults()));
    } catch (const realetting::error &e)
    {
        return std::unexpected(e.what());
    }
}

std::string Config::get(std::string_view key) const
{
    const nlohmann::json v = get_json(key);
    return v.is_string() ? v.get<std::string>() : std::string();
}

nlohmann::json Config::get_json(std::string_view key) const { return settings_[key].get(); }

// 档位间不回落：small_model 空就是空串
std::string Config::model(ModelTier tier) const
{
    return get(tier == ModelTier::Small ? "small_model" : "model");
}

bool Config::persist(std::string_view key, const nlohmann::json &v)
{
    try
    {
        settings_[key] = v;
        return true;
    } catch (const realetting::error &e)
    {
        fprintf(stderr, "[config] persist 放弃：%s\n", e.what());
        return false;
    }
}

std::string Config::models_path() const { return (global_dir() / "models.json").string(); }

} // namespace realagent
