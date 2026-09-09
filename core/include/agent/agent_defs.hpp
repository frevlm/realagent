/*
 * agent_defs.hpp — agent 定义：一段接在后面的文字（ADR-0024 §8）
 *
 * Claude Code 的 `agents/<name>.md` = frontmatter（name / description / tools / model）
 * + 正文（那个 subagent 的 system prompt）。**core 只当它是一段文字。**
 *
 * `spawn` 因此多一个可选参数 `agent`：给了名字，就把那份正文接在被派生 agent 的
 * system prompt 后面。不加新工具（ADR-0018 那条不变），不加执行路径。
 *
 * `tools` / `model` 两个字段**不收**：工具清单与模型档位不该由一段 markdown 决定
 * （同 command 那三样、同 ADR-0023「core 只取三样，其余自己写」）。
 *
 * **core 那段永远在前**（agent id / workdir / stop 契约）。这不需要额外保证：
 * Agent 只有一个类、一个 build_dialog，派生出来的走同一条路，拿不到「不带 stop 契约」
 * 的 system prompt。
 *
 * 由**派生方**解析：它在 system prompt 里看见了哪些名字，就该拿到哪一份正文。
 * 让被派生方按自己的 workdir 再查一次，会出现「模型看见的名字在那边不存在」。
 */
#pragma once

#include <string>
#include <vector>

namespace realagent {

struct AgentDef {
    std::string name;        // `<plugin>:<文件名>` 或就是文件名
    std::string description; // frontmatter 里的 description
    std::string body;        // 正文，${CLAUDE_PLUGIN_ROOT} 已展开
};

/* 走一遍扫描链，按名字排序返回。目录不存在不是错。 */
std::vector<AgentDef> scan_agent_defs(const std::string &workdir);

/* 拼进 system prompt 的那一段。**清单为空时返回空串**——没有 agent 定义的 agent，
 * 它的 system prompt 与加这个功能之前一个字不差（同 skills_prompt）。 */
std::string agent_defs_prompt(const std::vector<AgentDef> &defs);

} // namespace realagent
