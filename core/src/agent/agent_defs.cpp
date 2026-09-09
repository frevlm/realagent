/*
 * agent_defs.cpp — 扫盘
 *
 * 与 commands.cpp 同形：同一条扫描链、同一个 frontmatter 解析、同一条覆盖规则。
 * 差别只有目录名和它交给谁用。
 */
#include "agent/agent_defs.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "agent/frontmatter.hpp"
#include "plugin.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

void scan_dir(const PluginRoot &r, std::vector<AgentDef> &out)
{
    std::error_code ec;
    if (!fs::is_directory(r.agents_dir, ec)) return;
    std::vector<fs::path> files;
    for (const fs::directory_entry &e : fs::directory_iterator(r.agents_dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".md") files.push_back(e.path());
    std::sort(files.begin(), files.end()); // 顺序未指定，排一下：两次运行给同一张表

    for (const fs::path &md : files)
    {
        std::string err;
        const std::optional<Frontmatter> fm = read_frontmatter(md, true, err);
        if (!fm)
        {
            fprintf(stderr, "[agent-def] %s: %s，跳过\n", md.c_str(), err.c_str());
            continue;
        }
        const auto d = fm->fields.find("description");
        if (d == fm->fields.end() || d->second.empty())
        {
            // 没有描述，模型就没法知道什么时候该用它——那它进清单也是白进
            fprintf(stderr, "[agent-def] %s: 没有非空的 description，跳过\n", md.c_str());
            continue;
        }
        AgentDef a{r.qualify(md.stem().string()), d->second, fm->body};
        if (!expand_plugin_root(a.body, r.root))
        {
            fprintf(stderr, "[agent-def] %s: 正文里有 %s，它只在 plugin 里有意义，跳过\n",
                    md.c_str(), kPluginRootVar);
            continue;
        }
        const auto same = std::find_if(out.begin(), out.end(),
                                       [&](const AgentDef &o) { return o.name == a.name; });
        if (same != out.end())
            *same = std::move(a);
        else
            out.push_back(std::move(a));
    }
}

} // namespace

std::vector<AgentDef> scan_agent_defs(const std::string &workdir)
{
    std::vector<AgentDef> out;
    for (const PluginRoot &r : plugin_roots(workdir)) scan_dir(r, out);
    std::sort(out.begin(), out.end(),
              [](const AgentDef &a, const AgentDef &b) { return a.name < b.name; });
    return out;
}

std::string agent_defs_prompt(const std::vector<AgentDef> &defs)
{
    if (defs.empty()) return {};
    std::ostringstream s;
    s << "\n\nAgent definitions you can pass to `spawn` as the `agent` parameter. Each one is a "
         "role description that will be appended to the new agent's system prompt.\n";
    for (const AgentDef &a : defs) s << "- " << a.name << ": " << a.description << "\n";
    return s.str();
}

} // namespace realagent
