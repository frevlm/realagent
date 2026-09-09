#include "config.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace realagent {
namespace fs = std::filesystem;

namespace {

// 默认配置树（ADR-0010）：load() 用它打底，settings.json 再逐键覆盖。
// 只有一个键：permission。这是安全默认，缺了不该放行。
// 其余键（api_key / small_model / 端点那一束 base_url / model / protocol）没有默认——
// 缺了 get() 本就返回空串，再写一行 d["x"] = "" 是把编译器免费做的事运行期重做一遍；
// 端点三键为什么坚持不给默认，见 llm.hpp 与 endpoint_config_error()。
nlohmann::json defaults()
{
    // dangerous 工具执行前怎么裁决：ask 问用户（默认）/ allow-all 一律放行 / deny 一律拒绝
    //
    // mcp_http_bridge：`type: "http"` 的 MCP server 靠一个桥接进程说话（ADR-0024 §5）。
    // **这是一个真默认值**（同 ADR-0016「默认值可以是真的了」），装完即可用；
    // core 不硬编码包名——那会变成「core 决定去跑一个它没写的程序」，而 MCP 的判据是
    // 命令由用户在配置里点名。改成空数组 = 不支持 http，遇到报一条人话。
    //
    // 占位符 {url} / {name} / {value}。**子数组是「每个 header 重复一次」的那一组**，
    // 一个 header 都没有时整组不出现——边界写出来，不靠猜（`--header` 与它后面那个
    // 占位符是一组，这件事从参数本身看不出来）。
    return {{"permission", "ask"},
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

// 逐键覆盖：用户配了哪个键就换哪个键，没提的保留默认值。
// 配置树是平的（ADR-0016 删掉 plugins 那一节之后再没有嵌套键），所以不必递归——
// 需要嵌套的那天连着默认树一起加，不提前留机械。
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

// 不做档位间回落：small_model 空就是空串。回落会让"我明明配了小模型"与
// "我没配所以用了主模型"长得一模一样，出账单时才发现区别
std::string Config::model(ModelTier tier) const
{
    return get(tier == ModelTier::Small ? "small_model" : "model");
}

bool Config::persist(std::string_view key, const nlohmann::json &v)
{
    const fs::path target = settings_path(global_dir());

    // 点对点：读出文件原样，只改这一个键。不 dump 内存树——默认值不进用户的文件。
    auto file = read_settings(target);
    if (!file)
    {
        // 坏 JSON：拒绝写入。要写就只能整树覆盖，那会抹掉我们没读懂的用户数据（含 api_key）
        fprintf(stderr, "[config] persist 放弃：%s\n", file.error().c_str());
        return false;
    }
    nlohmann::json tree = file->value_or(nlohmann::json::object()); // 文件不存在 → 空对象起头
    tree[std::string(key)] = v;                                     // 只动这一个键，用户配的其余键原样留在文件里
    if (!write_atomic(target, tree.dump())) return false;

    // 落盘成功才改内存：失败时内存与文件都没变，不会出现"切了档但没写进去"
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
