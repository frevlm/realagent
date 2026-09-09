/*
 * commands.hpp — prompt 命令：plugin 带来的一段文字（ADR-0024 §7）
 *
 * **它不是一条命令，是一条消息的模板。** 内置那三条（`/new` / `/resume` / `/model`）
 * 是 core 的一个动作，拿锁、直接返回结果；这些是一段要发出去的话，展开后投收件箱、
 * 不拿锁——让它回 AGENT_BUSY，等于把发消息这件事变得比原来更难。
 *
 * 形状照抄 Claude Code，不发明第二种：`commands/<name>.md`，YAML frontmatter +
 * Markdown 正文。**只取三样**：`description`（`GET /commands` 要显示）、
 * `argument-hint`（菜单提示参数）、正文。`allowed-tools` / `model` 不收——
 * 权限与模型档位不该由一段 markdown 决定（同 ADR-0023「core 只取三样，其余自己写」）。
 *
 * 名字过 PluginRoot::qualify()：装来的是 `<plugin>:<文件名>`，隐式的就是文件名。
 * 于是**不同名的 plugin 之间撞不上**，而内置那三条只有隐式 plugin 撞得上——
 * 撞了就跳过并报出来，因为覆盖掉 `/new` 就没法开新会话。
 */
#pragma once

#include <string>
#include <vector>

namespace realagent {

struct PromptCommand {
    std::string name;          // 不带前导 '/'。`<plugin>:<文件名>` 或就是文件名
    std::string description;   // 没写就是空串——菜单少一句话，不是错
    std::string argument_hint; // frontmatter 的 `argument-hint`
    std::string body;          // 正文，${CLAUDE_PLUGIN_ROOT} 已展开
};

/* 走一遍扫描链，按名字排序返回。目录不存在不是错（多数人一条都没有）。
 * 坏文件跳过、错误原文进 stderr（`models.json` 那条先例）。
 * 撞上内置命令名的跳过——那三条不可被覆盖。 */
std::vector<PromptCommand> scan_commands(const std::string &workdir);

/* 展开 `$ARGUMENTS`。**不转义**：它进的是 prompt 不是 shell，与用户自己打那段字
 * 没有区别。正文里没有这个占位符时，参数附在末尾——不然用户打的字会凭空消失。 */
std::string expand_arguments(const std::string &body, const std::string &arg);

} // namespace realagent
