/*
 * skills.cpp — 扫盘、拼提示词
 *
 * frontmatter 的解析与出错时那句话在 frontmatter.cpp（`load_md`）——skill、command、
 * agent 定义是同一种文件，只写一份。这里只回答一件事：哪些目录里有 skill，它们叫什么。
 *
 * **正文一个字都不看**（`with_body=false`）：那是模型的事，core 只要一个 description。
 */
#include "agent/skills.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <sstream>

#include "agent/frontmatter.hpp"
#include "plugin.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 读一份 SKILL.md 的 description。读不出来就报错原文、返回 nullopt，
 * 调用方跳过这一个——skill 是 N 份各自独立的文件，一份坏了不让另一份变得可疑。 */
std::optional<std::string> read_description(const fs::path &md, const PluginRoot &r)
{
    const std::optional<Frontmatter> fm = load_md(md, r, "skill", false);
    if (!fm) return std::nullopt;
    const auto it = fm->fields.find("description");
    if (it == fm->fields.end() || it->second.empty())
    {
        fprintf(stderr, "[skill] %s: frontmatter 里没有非空的字符串 description，跳过\n",
                md.c_str());
        return std::nullopt;
    }
    return it->second;
}

/* 扫一站。同名的后来者覆盖先到者——调用方按「远的先扫」的顺序调，近的就赢了。
 *
 * 名字过一遍 `qualify()`：装来的 plugin 得到 `<plugin>:<目录名>`，隐式的原样。
 * 于是**不同名的 plugin 之间永远撞不上**，覆盖只发生在同一个 plugin 的两份之间。 */
void scan_dir(const PluginRoot &r, std::vector<Skill> &out)
{
    std::error_code ec;
    if (!fs::is_directory(r.skills_dir, ec)) return; // 多数人一个 skill 都没有
    for (const fs::directory_entry &e : fs::directory_iterator(r.skills_dir, ec))
    {
        if (!e.is_directory(ec)) continue;
        const fs::path md = e.path() / "SKILL.md";
        if (!fs::is_regular_file(md, ec)) continue; // 目录里没有 SKILL.md：它就不是 skill
        const std::optional<std::string> desc = read_description(md, r);
        if (!desc) continue;
        Skill s{r.qualify(e.path().filename().string()), *desc, fs::absolute(md, ec).string()};
        const auto it = std::find_if(out.begin(), out.end(),
                                     [&](const Skill &o) { return o.name == s.name; });
        if (it != out.end())
            *it = std::move(s);
        else
            out.push_back(std::move(s));
    }
}

} // namespace

std::vector<Skill> scan_skills(const std::string &workdir)
{
    std::vector<Skill> out;
    // 顺序遍历扫描链：远的在前、近的在后，同名 skill 就此被近的那份盖掉（ADR-0024 §1）
    for (const PluginRoot &r : plugin_roots(workdir)) scan_dir(r, out);
    // directory_iterator 的顺序是未指定的，排一下——同一个目录两次运行给同一份提示词
    std::sort(out.begin(), out.end(), [](const Skill &a, const Skill &b) { return a.name < b.name; });
    return out;
}

std::string skills_prompt(const std::vector<Skill> &skills)
{
    if (skills.empty()) return {};
    std::ostringstream s;
    s << "\n\nSkills available to you. Each one is a Markdown document of instructions. "
         "When a description matches what you are about to do, `read` that file first and "
         "follow it.\n";
    for (const Skill &k : skills) s << "- " << k.name << ": " << k.description << " (" << k.path << ")\n";
    return s.str();
}

} // namespace realagent
