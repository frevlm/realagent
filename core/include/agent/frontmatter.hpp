/*
 * frontmatter.hpp — YAML frontmatter + Markdown 正文
 *
 * skill（ADR-0022）与 command（ADR-0024 §7）是同一种文件：首行 `---`，到下一条 `---`
 * 为止是 YAML，其余是正文。**解析只写一份**——抄两遍就会有它们解析得不一样的那天。
 *
 * 用 vendored 的 fkYAML 解析，不手写：这些文件是从互联网抄来的第三方输入，形状不由
 * core 说了算。手写的解析器碰上 `description: >`（折叠标量，现实里占三成）不会报错，
 * 它会安静地把值设成 `>`。「不兜底」反对的是替用户擦屁股，不是反对按格式的真实定义解析。
 *
 * **只收字符串标量**，别的类型直接跳过：core 从这些文件里要的全是字符串
 * （`description` / `argument-hint`），一个都不是别的。
 */
#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "plugin.hpp"

namespace realagent {

struct Frontmatter {
    std::map<std::string, std::string> fields; // 只有字符串标量
    std::string body;                          // with_body 为假时是空串
};

/* 读一份 md。没有 frontmatter、YAML 解析失败、根本不是 mapping —— 都返回 nullopt
 * 并把原话写进 err（调用方跳过这一份，不牵连别的）。
 *
 * **`with_body` 是一条真区别，不是一个旋钮**：skill 的正文归模型，core 一个字都不看
 * （ADR-0022）；command 的正文归 core，它就是要发出去的那段话（ADR-0024 §7）。 */
std::optional<Frontmatter> read_frontmatter(const std::filesystem::path &md, bool with_body,
                                            std::string &err);

/* 一个 `.md` 目录里的文件：`<qualify 过的名字, 路径>`，**排过序**。
 * directory_iterator 的顺序是未指定的——不排，同一个目录两次运行给出的表就不一样。
 * 目录不存在返回空表，不是错：多数人一条 command、一份 agent 定义都没有。 */
std::vector<std::pair<std::string, std::filesystem::path>>
md_files(const PluginRoot &r, const std::filesystem::path &dir);

/* 读一份 md，并把正文里的 `${CLAUDE_PLUGIN_ROOT}` 展开。
 *
 * 读不出来、展开不了都返回 nullopt，原话进 stderr（`tag` 是方括号里那个词）——
 * 这些文件是 N 份各自独立的输入，**一份坏了不让同目录的别人变得可疑**。
 *
 * 报错与展开这两步三个扫描器一字不差，所以只写在这儿；「哪些字段作数」各扫各的。 */
std::optional<Frontmatter> load_md(const std::filesystem::path &md, const PluginRoot &r,
                                   const char *tag, bool with_body);

} // namespace realagent
