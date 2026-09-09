/*
 * commands.cpp — 扫盘、展开
 *
 * frontmatter 的解析在 frontmatter.cpp，与 skill 共用一份。这里只回答：
 * 哪些目录里有 prompt 命令，它们叫什么，正文是什么。
 */
#include "agent/commands.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "agent/command.hpp"
#include "agent/frontmatter.hpp"
#include "plugin.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 扫一站。同名的后来者覆盖先到者——调用方按「远的先扫」的顺序调，近的就赢了。 */
void scan_dir(const PluginRoot &r, std::vector<PromptCommand> &out)
{
    std::error_code ec;
    if (!fs::is_directory(r.commands_dir, ec)) return; // 多数人一条都没有
    std::vector<fs::path> files;
    for (const fs::directory_entry &e : fs::directory_iterator(r.commands_dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md") files.push_back(e.path());
    // directory_iterator 的顺序是未指定的，排一下：同一个目录两次运行给同一张表
    std::sort(files.begin(), files.end());

    for (const fs::path &md : files)
    {
        const std::string name = r.qualify(md.stem().string());
        /* 内置那三条不可被覆盖：覆盖掉 `/new` 就没法开新会话（同内置六个工具的理由）。
         * 装来的带前缀，天然撞不上——撞得上的只有隐式 plugin。 */
        if (is_builtin_command(name))
        {
            fprintf(stderr, "[command] %s: `%s` 是内置命令，不可覆盖，跳过\n", md.c_str(),
                    name.c_str());
            continue;
        }
        std::string err;
        const std::optional<Frontmatter> fm = read_frontmatter(md, true, err);
        if (!fm)
        {
            fprintf(stderr, "[command] %s: %s，跳过\n", md.c_str(), err.c_str());
            continue;
        }
        PromptCommand c{name, {}, {}, fm->body};
        if (const auto it = fm->fields.find("description"); it != fm->fields.end())
            c.description = it->second;
        if (const auto it = fm->fields.find("argument-hint"); it != fm->fields.end())
            c.argument_hint = it->second;

        /* 正文里的 ${CLAUDE_PLUGIN_ROOT} 与 .mcp.json 里那个是同一条判据、同一个白名单：
         * 现成的 command 常引用自己目录里的脚本（ADR-0024 §4）。 */
        if (!expand_plugin_root(c.body, r.root))
        {
            fprintf(stderr, "[command] %s: 正文里有 %s，它只在 plugin 里有意义，跳过\n",
                    md.c_str(), kPluginRootVar);
            continue;
        }
        const auto same = std::find_if(out.begin(), out.end(),
                                       [&](const PromptCommand &o) { return o.name == c.name; });
        if (same != out.end())
            *same = std::move(c);
        else
            out.push_back(std::move(c));
    }
}

} // namespace

std::vector<PromptCommand> scan_commands(const std::string &workdir)
{
    std::vector<PromptCommand> out;
    // 顺序遍历扫描链：远的在前、近的在后，同名被近的那份盖掉（ADR-0024 §1）
    for (const PluginRoot &r : plugin_roots(workdir)) scan_dir(r, out);
    std::sort(out.begin(), out.end(),
              [](const PromptCommand &a, const PromptCommand &b) { return a.name < b.name; });
    return out;
}

std::string expand_arguments(const std::string &body, const std::string &arg)
{
    static constexpr std::string_view kVar = "$ARGUMENTS";
    if (body.find(kVar) == std::string::npos)
        // 正文没写占位符：附在末尾。丢掉用户打的字是最坏的一种「什么都没发生」
        return arg.empty() ? body : body + "\n\n" + arg;
    std::string out = body;
    for (size_t at = out.find(kVar); at != std::string::npos; at = out.find(kVar, at + arg.size()))
        out.replace(at, kVar.size(), arg);
    return out;
}

} // namespace realagent
