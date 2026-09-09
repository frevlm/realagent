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

} // namespace realagent
