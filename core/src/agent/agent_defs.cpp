/*
 * agent_defs.cpp — 扫盘
 *
 * 与 commands.cpp 同形：同一条扫描链、同一份 `md_files` / `load_md`、同一条覆盖规则。
 * 差别只有目录名、哪些字段作数，和它交给谁用——**这里只剩这三样**。
 */
#include "agent/agent_defs.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "agent/frontmatter.hpp"
#include "plugin.hpp"

namespace realagent {

namespace {

void scan_dir(const PluginRoot &r, std::vector<AgentDef> &out)
{
    for (const auto &[name, md] : md_files(r, r.agents_dir))
    {
        std::optional<Frontmatter> fm = load_md(md, r, "agent-def", true);
        if (!fm) continue;
        const auto d = fm->fields.find("description");
        if (d == fm->fields.end() || d->second.empty())
        {
            // 没有描述，模型就没法知道什么时候该用它——那它进清单也是白进
            fprintf(stderr, "[agent-def] %s: 没有非空的 description，跳过\n", md.c_str());
            continue;
        }
        AgentDef a{name, d->second, std::move(fm->body)};
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
