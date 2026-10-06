#include "agent/catalog.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

#include "agent/command.hpp"
#include "plugin.hpp"

/* fkYAML 是逐字节 vendored 的，它用了 C++26 弃用的 std::is_trivial */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#include "fkYAML.hpp"
#pragma clang diagnostic pop

namespace realagent {

namespace fs = std::filesystem;

namespace {

struct Frontmatter {
    std::map<std::string, std::string> fields; // 只收字符串标量
    std::string body;

    std::string get(const std::string &k) const
    {
        const auto it = fields.find(k);
        return it == fields.end() ? std::string() : it->second;
    }
};

/* 去掉行尾空白：`---\r` 也是 `---` */
std::string_view rstrip(std::string_view s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

/* 切出首行 `---` 到下一个 `---` 之间的 YAML，交给 fkYAML。失败返回 nullopt 并写 err。 */
std::optional<Frontmatter> parse(const fs::path &md, bool with_body, std::string &err)
{
    std::ifstream f(md);
    std::string line;
    if (!std::getline(f, line) || rstrip(line) != "---")
    {
        err = "没有 YAML frontmatter";
        return std::nullopt;
    }
    std::string yaml;
    bool closed = false;
    while (!closed && std::getline(f, line))
        if (!(closed = rstrip(line) == "---")) yaml += line + '\n';
    if (!closed)
    {
        err = "frontmatter 开了头没收尾";
        return std::nullopt;
    }

    Frontmatter out;
    if (with_body)
    {
        std::ostringstream body;
        body << f.rdbuf();
        out.body = body.str();
    }
    try
    {
        const fkyaml::node n = fkyaml::node::deserialize(yaml);
        if (!n.is_mapping())
        {
            err = "frontmatter 不是一组键值";
            return std::nullopt;
        }
        for (const auto &[k, v] : n.as_map())
            if (v.is_string()) out.fields[k.as_str()] = v.get_value<std::string>();
    } catch (const std::exception &e)
    {
        err = std::string("frontmatter 解析失败：") + e.what();
        return std::nullopt;
    }
    return out;
}

/* 读一份 md 并展开正文里的 ${CLAUDE_PLUGIN_ROOT}。出错报一行、返回 nullopt。
 * skill 的正文归模型，core 不读（with_body = false）。 */
std::optional<Frontmatter> load(const fs::path &md, const PluginRoot &r, const char *tag,
                                bool with_body = true)
{
    std::string err;
    std::optional<Frontmatter> fm = parse(md, with_body, err);
    if (fm && !expand_plugin_root(fm->body, r.root))
        err = std::string("正文里有 ") + kPluginRootVar + "，它只在 plugin 里有意义";
    if (!err.empty())
    {
        fprintf(stderr, "[%s] %s: %s，跳过\n", tag, md.c_str(), err.c_str());
        return std::nullopt;
    }
    return fm;
}

/* 带非空 description 才收：没描述，模型就不知道什么时候该用它。 */
std::optional<std::string> required_description(const Frontmatter &fm, const fs::path &md,
                                                const char *tag)
{
    std::string d = fm.get("description");
    if (d.empty()) fprintf(stderr, "[%s] %s: 没有非空的 description，跳过\n", tag, md.c_str());
    return d.empty() ? std::nullopt : std::optional(d);
}

/* 目录下的 `*.md`：<qualify 过的文件名, 路径>。 */
std::vector<std::pair<std::string, fs::path>> md_files(const PluginRoot &r, const fs::path &dir)
{
    std::error_code ec;
    std::vector<std::pair<std::string, fs::path>> out;
    if (!fs::is_directory(dir, ec)) return out;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md")
            out.emplace_back(r.qualify(e.path().stem().string()), e.path());
    return out;
}

/* 走扫描链，每站交给 one(r, add) 产出若干项。同名时后来者（近的）覆盖，map 顺带排好序。 */
template <class T, class F>
std::vector<T> scan_chain(const std::string &workdir, F one)
{
    std::map<std::string, T> by_name;
    const auto add = [&by_name](T x) { by_name.insert_or_assign(x.name, std::move(x)); };
    for (const PluginRoot &r : plugin_roots(workdir)) one(r, add);
    std::vector<T> out;
    for (auto &[_, x] : by_name) out.push_back(std::move(x));
    return out;
}

} // namespace

std::vector<Skill> scan_skills(const std::string &workdir)
{
    return scan_chain<Skill>(workdir, [](const PluginRoot &r, const auto &add) {
        std::error_code ec;
        if (!fs::is_directory(r.skills_dir, ec)) return;
        for (const fs::directory_entry &e : fs::directory_iterator(r.skills_dir, ec))
        {
            const fs::path md = e.path() / "SKILL.md";
            if (!e.is_directory(ec) || !fs::is_regular_file(md, ec)) continue;
            const auto fm = load(md, r, "skill", false);
            const auto desc = fm ? required_description(*fm, md, "skill") : std::nullopt;
            if (desc) add(Skill{r.qualify(e.path().filename().string()), *desc, fs::absolute(md, ec).string()});
        }
    });
}

std::vector<PromptCommand> scan_commands(const std::string &workdir)
{
    return scan_chain<PromptCommand>(workdir, [](const PluginRoot &r, const auto &add) {
        for (const auto &[name, md] : md_files(r, r.commands_dir))
        {
            // 覆盖掉 /new 就没法开新会话
            if (is_builtin_command(name))
            {
                fprintf(stderr, "[command] %s: `%s` 是内置命令，不可覆盖，跳过\n", md.c_str(),
                        name.c_str());
                continue;
            }
            if (auto fm = load(md, r, "command"))
                add(PromptCommand{name, fm->get("description"), fm->get("argument-hint"),
                                  std::move(fm->body)});
        }
    });
}

std::vector<AgentDef> scan_agent_defs(const std::string &workdir)
{
    return scan_chain<AgentDef>(workdir, [](const PluginRoot &r, const auto &add) {
        for (const auto &[name, md] : md_files(r, r.agents_dir))
        {
            auto fm = load(md, r, "agent-def");
            const auto desc = fm ? required_description(*fm, md, "agent-def") : std::nullopt;
            if (desc) add(AgentDef{name, *desc, std::move(fm->body)});
        }
    });
}

std::string skills_prompt(const std::vector<Skill> &skills)
{
    if (skills.empty()) return {};
    std::string s = "\n\nSkills available to you. Each one is a Markdown document of instructions. "
                    "When a description matches what you are about to do, `read` that file first and "
                    "follow it.\n";
    for (const Skill &k : skills) s += "- " + k.name + ": " + k.description + " (" + k.path + ")\n";
    return s;
}

std::string agent_defs_prompt(const std::vector<AgentDef> &defs)
{
    if (defs.empty()) return {};
    std::string s = "\n\nAgent definitions you can pass to `spawn` as the `agent` parameter. Each one is "
                    "a role description that will be appended to the new agent's system prompt.\n";
    for (const AgentDef &a : defs) s += "- " + a.name + ": " + a.description + "\n";
    return s;
}

std::string expand_arguments(const std::string &body, const std::string &arg)
{
    static constexpr std::string_view kVar = "$ARGUMENTS";
    if (body.find(kVar) == std::string::npos) return arg.empty() ? body : body + "\n\n" + arg;
    std::string out = body;
    for (size_t at = out.find(kVar); at != std::string::npos; at = out.find(kVar, at + arg.size()))
        out.replace(at, kVar.size(), arg);
    return out;
}

} // namespace realagent
