/*
 * tools.hpp — 内置工具：一张表、按名派发，外加每个工具自己的定义与实现
 *
 * 一个工具一个文件（src/tools/*.cpp），描述与实现住在一起。
 * spawn / send_message 只有定义：它们要认识 Agents，实现在 Executor 里。
 * edit 只有「把一段行范围换成一段文本」一个操作，创建文件也是它（ADR-0018）。
 */
#pragma once

#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "agent/context.hpp"
#include "json.hpp"

namespace realagent {

/* 工具定义就是端点要的那个对象（name / description / input_schema），外加 `_core`
 * 装 core 私有字段：label、dangerous，MCP 来的还有 server、remote_name。
 * 发给端点前 erase("_core")。不叫 `_meta`：那是 MCP 自己的保留字段。 */
nlohmann::json tool_def(std::string name, std::string label, std::string description,
                        std::string_view input_schema, bool dangerous);

/* `_core` 由 core 自己写齐，缺了就是 bug，所以不给默认值。 */
inline bool tool_dangerous(const nlohmann::json &t) { return t["_core"]["dangerous"]; }

/* 空 = 代码在 core 里；非空 = 跑在那个 MCP server 的进程里。 */
inline std::string tool_server(const nlohmann::json &t)
{
    return t["_core"].value("server", std::string());
}

inline const nlohmann::json *find_by_name(const nlohmann::json &table, std::string_view name)
{
    for (const nlohmann::json &t : table)
        if (t.value("name", std::string()) == name) return &t;
    return nullptr;
}

/* 内置工具清单，顺序即模型看到的顺序。MCP 来的不在这里。 */
const nlohmann::json &tool_defs();
const nlohmann::json *find_tool(std::string_view name);

/* 执行内置的 read / edit / bash。call_id 进实时输出帧；相对路径从 workdir 算起；
 * abort 变真时 bash 杀掉子进程组。 */
nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const nlohmann::json &params, const EmitFn &emit,
                        const std::string &workdir, const std::atomic<bool> *abort = nullptr);

/* —— 结果：`{"content": [块...], "isError": bool}`，MCP 定的形状 —— */

inline nlohmann::json tool_result(bool is_error, nlohmann::json content)
{
    return nlohmann::json{{"content", std::move(content)}, {"isError", is_error}};
}
inline nlohmann::json tool_text(bool is_error, std::string text)
{
    return tool_result(is_error,
                       nlohmann::json::array({{{"type", "text"}, {"text", std::move(text)}}}));
}
inline nlohmann::json tool_fail(const std::string &msg) { return tool_text(true, msg); }
inline nlohmann::json tool_ok(const std::string &what) { return tool_text(false, what); }

/* 块数组 → 纯文本，给只收字符串的去处（OpenAI 两套协议、回放帧）。
 * 非文本块压成一行占位，说清是什么、多大，不假装它在。 */
inline std::string tool_content_text(const nlohmann::json &content)
{
    std::string out;
    for (const nlohmann::json &b : content)
    {
        const std::string type = b.value("type", std::string());
        if (type == "text")
        {
            out += b.value("text", std::string());
            continue;
        }
        if (type == "resource")
        {
            const auto r = b.find("resource");
            if (r != b.end() && r->is_object() && r->contains("text"))
            {
                out += r->value("text", std::string());
                continue;
            }
        }
        if (!out.empty() && out.back() != '\n') out += '\n';
        if (type == "resource_link")
        {
            out += "[resource_link " + b.value("uri", std::string()) + "]";
            continue;
        }
        const size_t bytes = b.value("data", std::string()).size() / 4 * 3; // base64 4→3
        out += "[" + b.value("mimeType", type.empty() ? std::string("?") : type) + ", " +
               (bytes >= 1024 ? std::to_string(bytes / 1024) + " KB" : std::to_string(bytes) + " B") +
               " —— 这里带不动]";
    }
    return out;
}

/* 字符串参数；缺失或不是字符串返回 nullopt。 */
inline std::optional<std::string> tool_arg(const nlohmann::json &params, std::string_view key)
{
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/* 相对路径从 agent 的工作目录算起，不看 core 进程的 cwd。 */
inline std::string tool_resolve(const std::string &workdir, const std::string &path)
{
    const std::filesystem::path p(path);
    return p.is_absolute() ? path : (std::filesystem::path(workdir) / p).string();
}

/* —— 行与 anchor（ADR-0018）：read 打印、edit 校验，同一份契约 —— */

inline std::vector<std::string> read_lines(const std::string &path)
{
    std::vector<std::string> lines;
    std::ifstream f(path);
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    return lines;
}

inline bool write_lines(const std::string &path, const std::vector<std::string> &lines)
{
    std::ofstream f(path, std::ios::trunc);
    for (const std::string &l : lines) f << l << '\n';
    return f.good();
}

/* 一行的 hash：FNV-1a 取 3 个十六进制字符。不算空白，格式化不改变它。 */
inline std::string hash_line(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        if (!std::isspace(c)) h = (h ^ c) * 16777619u;
    char buf[4];
    std::snprintf(buf, sizeof buf, "%03x", h & 0xfff);
    return buf;
}

/* —— 每个工具：定义 + 实现 —— */

nlohmann::json read_def();
nlohmann::json read_run(const nlohmann::json &params, const std::string &workdir);

nlohmann::json edit_def();
nlohmann::json edit_run(const nlohmann::json &params, const std::string &workdir);

nlohmann::json bash_def();
nlohmann::json bash_run(const std::string &call_id, const nlohmann::json &params,
                        const EmitFn &emit, const std::string &workdir,
                        const std::atomic<bool> *abort);

nlohmann::json spawn_def();
nlohmann::json send_message_def();

} // namespace realagent
