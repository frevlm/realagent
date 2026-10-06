/*
 * plugin.cpp — 扫描链：按什么顺序走过哪几站（读什么归各扫描器）
 */
#include "plugin.hpp"

#include <algorithm>
#include <string>

#include <fstream>

#include "config.hpp"
#include "json.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 一处 plugins/ 目录下的全部子目录，按名排序（directory_iterator 的顺序未指定，
 * 而它决定同名时谁盖谁）。不要求有 plugin.json。 */
void scan_plugins_dir(const fs::path &dir, std::vector<PluginRoot> &out)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return; // 多数人一个 plugin 都没装
    std::vector<fs::path> roots;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec)) roots.push_back(e.path());
    std::sort(roots.begin(), roots.end());
    for (const fs::path &r : roots)
        // 装来的用 Claude Code 的文件名，不发明第二种（ADR-0024 §1）
        out.push_back({r.filename().string(), r, r / "skills", r / "commands", r / "agents",
                       r / "hooks" / "hooks.json", r / ".mcp.json"});
}

} // namespace

bool expand_plugin_root(std::string &s, const fs::path &root)
{
    const std::string var = kPluginRootVar;
    if (s.find(var) == std::string::npos) return true; // 没出现：什么都不做也是成功
    if (root.empty()) return false;                    // 隐式 plugin 那儿没有 plugin root
    const std::string to = root.string();
    for (size_t at = s.find(var); at != std::string::npos; at = s.find(var, at + to.size()))
        s.replace(at, var.size(), to);
    return true;
}

std::vector<PluginRoot> plugin_roots(const std::string &workdir)
{
    const fs::path home = fs::path(getenv_or("HOME", ".")) / ".realagent";
    const fs::path proj = fs::path(workdir) / ".realagent";

    std::vector<PluginRoot> out;
    // 装来的在前、隐式在后。跨前缀撞不上，这个顺序只在前缀相同的两站之间起作用
    scan_plugins_dir(home / "plugins", out);
    scan_plugins_dir(proj / "plugins", out);
    // 隐式那两个在后，root 留空 = 没有前缀、也没有 ${CLAUDE_PLUGIN_ROOT}
    out.push_back({"user", {}, home / "skills", home / "commands", home / "agents", home / "hooks" / "hooks.json", home / "mcp.json"});
    out.push_back({"project", {}, proj / "skills", proj / "commands", proj / "agents", proj / "hooks" / "hooks.json", proj / "mcp.json"});
    return out;
}

/* 数一个目录里有几个 <name>/SKILL.md */
static int count_skills(const fs::path &dir)
{
    std::error_code ec;
    int n = 0;
    if (!fs::is_directory(dir, ec)) return 0;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec) && fs::is_regular_file(e.path() / "SKILL.md", ec)) ++n;
    return n;
}

/* 数一个目录里有几个 .md */
static int count_md(const fs::path &dir)
{
    std::error_code ec;
    int n = 0;
    if (!fs::is_directory(dir, ec)) return 0;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md") ++n;
    return n;
}

/* 数一份 JSON 文件里某个对象有几个键。读不动就是 0——**这里不报错**：
 * 坏文件的原话由各自的扫描器在 agent 创建时报过了。 */
static int count_keys(const fs::path &file, const char *outer, const char *inner)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return 0;
    std::ifstream f(file);
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return 0;
    const nlohmann::json &o = inner && j.contains(inner) ? j[inner] : j;
    const auto it = o.find(outer);
    return it != o.end() && it->is_object() ? (int)it->size() : 0;
}

/* 数一份 hooks.json 里挂了几条 hook——**不是几个事件**：一个事件下可以挂好几条，
 * 而用户想知道的是「有几个程序会跑」。 */
static int count_hooks(const fs::path &file)
{
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return 0;
    std::ifstream f(file);
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return 0;
    const nlohmann::json &groups = j.contains("hooks") ? j["hooks"] : j;
    if (!groups.is_object()) return 0;
    int n = 0;
    for (const auto &[ev, arr] : groups.items())
    {
        if (!arr.is_array()) continue;
        for (const auto &g : arr)
            if (const auto it = g.find("hooks"); it != g.end() && it->is_array())
                n += (int)it->size();
    }
    return n;
}

std::vector<PluginInfo> plugin_infos(const std::string &workdir)
{
    std::vector<PluginInfo> out;
    for (const PluginRoot &r : plugin_roots(workdir))
    {
        PluginInfo p;
        p.name = r.name;
        p.implicit = r.root.empty();
        p.root = (p.implicit ? r.skills_dir.parent_path() : r.root).string();
        p.skills = count_skills(r.skills_dir);
        p.commands = count_md(r.commands_dir);
        p.agent_defs = count_md(r.agents_dir);
        p.mcp_servers = count_keys(r.mcp_file, "mcpServers", nullptr);
        p.hooks = count_hooks(r.hooks_file);

        // 名片。名字仍旧取目录名——文件系统已经保证唯一（ADR-0024 §2）
        std::error_code ec;
        if (const fs::path card = r.root / ".claude-plugin" / "plugin.json";
            !p.implicit && fs::is_regular_file(card, ec))
        {
            std::ifstream f(card);
            const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_object())
            {
                p.description = j.value("description", std::string());
                p.version = j.value("version", std::string());
            }
        }
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace realagent
