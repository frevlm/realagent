#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace realagent {
namespace fs = std::filesystem;

namespace {

// 默认配置树（ADR-0010）。没列出的键缺省为空串。
nlohmann::json defaults()
{
    // permission：ask（问用户）/ allow-all / deny。
    // mcp_http_bridge：http 型 MCP server 的桥接命令模板（ADR-0024 §5），空数组 = 不支持 http。
    // 占位符 {url} / {name} / {value}；子数组是每个 header 重复一次的那一组。
    // setup_done：首启引导走完过没有。false 时 TUI 启动先跑引导（GET / POST /setup）。
    return {{"permission", "ask"},
            {"setup_done", false},
            {"mcp_http_bridge",
             {"npx", "-y", "mcp-remote", "{url}",
              nlohmann::json::array({"--header", "{name}: {value}"})}}};
}

fs::path settings_path(const fs::path &dir) { return dir / ".realagent" / "settings.json"; }

// 全局配置落点：~/.realagent/settings.json——唯一的覆盖来源，不看 cwd
fs::path global_dir() { return fs::path(getenv_or("HOME", ".")); }

// 读一份 settings.json。文件不存在 → nullopt（不是错误，用默认树就行）；
// 打不开 / 解析不了 → 错误（读不懂就别猜）
std::expected<std::optional<nlohmann::json>, std::string> read_settings(const fs::path &path)
{
    if (!fs::exists(path)) return std::optional<nlohmann::json>{};
    std::ifstream f(path);
    if (!f) return std::unexpected(path.string() + " 打不开");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // 第三个参数 false = 解析失败不抛，返回一个 discarded 值
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded()) return std::unexpected(path.string() + " 不是合法 JSON");
    return std::optional<nlohmann::json>{std::move(j)};
}

// 逐键覆盖。配置树是平的，不必递归。
void merge_into(nlohmann::json &dst, const nlohmann::json &src)
{
    if (!src.is_object()) return; // settings.json 是合法 JSON 但不是对象：当没配
    for (const auto &[k, v] : src.items()) dst[k] = v;
}

// tmp + rename 原子写。断电或进程被杀只会留下临时文件，不会留半截的 settings.json
bool write_atomic(const fs::path &target, const std::string &text)
{
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    if (ec)
    {
        fprintf(stderr, "[config] persist: 创建目录失败 %s\n", ec.message().c_str());
        return false;
    }
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f)
        {
            fprintf(stderr, "[config] persist: 无法写 %s\n", tmp.c_str());
            return false;
        }
        f << text << "\n";
    }
    // 里面有 api_key：只给自己读写
    fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, ec);
    fs::rename(tmp, target, ec);
    if (ec)
    {
        fprintf(stderr, "[config] persist: rename 失败 %s\n", ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

std::string getenv_or(std::string_view name, std::string_view fallback)
{
    if (const char *v = std::getenv(std::string(name).c_str()); v != nullptr)
        return std::string(v);
    return std::string(fallback);
}

std::expected<Config, std::string> Config::load()
{
    Config cfg;
    cfg.settings_ = defaults();

    // 默认树打底，settings.json 覆盖。缺键不是错，缺的就用默认值——不校验必需项。
    auto file = read_settings(settings_path(global_dir()));
    if (!file) return std::unexpected(file.error());
    if (*file) merge_into(cfg.settings_, **file);

    return cfg;
}

std::string Config::get(std::string_view key) const
{
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_.value(std::string(key), std::string());
}

nlohmann::json Config::get_json(std::string_view key) const
{
    std::lock_guard<std::mutex> lk(*mutex_);
    const auto it = settings_.find(std::string(key));
    return it == settings_.end() ? nlohmann::json() : *it;
}

bool Config::has(std::string_view key) const
{
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_.contains(std::string(key));
}

// 档位间不回落：small_model 空就是空串
std::string Config::model(ModelTier tier) const
{
    return get(tier == ModelTier::Small ? "small_model" : "model");
}

bool Config::persist(std::string_view key, const nlohmann::json &v)
{
    const fs::path target = settings_path(global_dir());

    auto file = read_settings(target);
    if (!file)
    {
        // 坏 JSON 拒绝写入：不能盖掉读不懂的用户数据
        fprintf(stderr, "[config] persist 放弃：%s\n", file.error().c_str());
        return false;
    }
    nlohmann::json tree = file->value_or(nlohmann::json::object());
    tree[std::string(key)] = v; // 只动这一个键
    if (!write_atomic(target, tree.dump())) return false;

    // 落盘成功才改内存
    std::lock_guard<std::mutex> lk(*mutex_);
    settings_[std::string(key)] = v;
    return true;
}

std::string Config::models_path() const
{
    return (global_dir() / ".realagent" / "models.json").string();
}

nlohmann::json Config::to_json() const
{
    std::lock_guard<std::mutex> lk(*mutex_);
    return settings_;
}

} // namespace realagent
