/*
 * catalog.hpp — 扫描链上的三种 Markdown：skill、prompt 命令、agent 定义
 *
 * 三种都是 YAML frontmatter + Markdown 正文，形状照抄 Claude Code / Agent Skills 规范。
 * 走同一条扫描链（plugin_roots，远的先、近的后），同名时近的盖掉远的，结果按名排序。
 * 名字过 PluginRoot::qualify()：装来的 plugin 带 `<plugin>:` 前缀，隐式的不带。
 * 坏文件跳过，原因进 stderr。目录不存在不是错。
 *
 * 扫盘在 agent 创建时发生一次：中途变了 system prompt，prompt cache 就碎了。
 */
#pragma once

#include <string>
#include <vector>

namespace realagent {

/* skill（ADR-0022）：一个目录 + SKILL.md。core 只把名字、描述、路径写进 system prompt，
 * 正文由模型自己用 read 去读。 */
struct Skill {
    std::string name; // 目录名
    std::string description;
    std::string path; // SKILL.md 的绝对路径
};

/* prompt 命令（ADR-0024 §7）：`commands/<name>.md`，一条消息的模板。
 * 展开后投收件箱。撞上内置命令名的跳过。 */
struct PromptCommand {
    std::string name; // 不带前导 '/'
    std::string description;
    std::string argument_hint; // frontmatter 的 `argument-hint`
    std::string body;          // ${CLAUDE_PLUGIN_ROOT} 已展开
};

/* agent 定义（ADR-0024 §8）：`agents/<name>.md`，正文接在被 spawn 的 agent 的
 * system prompt 后面。frontmatter 里的 tools / model 不收。 */
struct AgentDef {
    std::string name;
    std::string description;
    std::string body; // ${CLAUDE_PLUGIN_ROOT} 已展开
};

std::vector<Skill> scan_skills(const std::string &workdir);
std::vector<PromptCommand> scan_commands(const std::string &workdir);
std::vector<AgentDef> scan_agent_defs(const std::string &workdir);

/* 拼进 system prompt 的段落。清单为空返回空串。 */
std::string skills_prompt(const std::vector<Skill> &skills);
std::string agent_defs_prompt(const std::vector<AgentDef> &defs);

/* 展开 `$ARGUMENTS`（不转义：进的是 prompt 不是 shell）。
 * 正文里没有占位符时参数附在末尾，用户打的字不会消失。 */
std::string expand_arguments(const std::string &body, const std::string &arg);

} // namespace realagent
