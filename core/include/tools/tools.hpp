/*
 * tools.hpp — 内置工具：一张表、按名派发、中止，外加每个工具自己那两件事
 *
 * 一个工具一个文件（src/tools/read.cpp、edit.cpp、bash.cpp、spawn.cpp、
 * send_message.cpp）：发给 LLM 的那段描述与它的实现住在一起，改了行为忘了改描述
 * 就没有缝可钻。tools.cpp 只剩壳——把这些定义串成表，按名字派发。
 *
 * spawn / send_message 只有定义没有实现：它们要认识 Agents，实现在 Executor 里
 * （tools/ 在 agent/ 下面，反过来包含就是层级倒挂）。定义仍旧在这张表里，
 * 因为 LLM 看见的工具清单只有一份。
 *
 * edit 只有一个操作：把一段行范围换成一段文本（范围由 [[Anchor]] 指定，两端闭）。
 * 改、删、插、创建是它的四种用法，不是四个操作——创建就是「文件不存在，没有 anchor
 * 可给」那一格，所以**没有独立的 write 工具**（ADR-0018）。
 *
 * 中止（ADR-0002 R8）：只有 bash 需要——它是唯一会跑很久的那个，所以 interrupt_tool()
 * 就定义在 bash.cpp 里，它要动的状态全在那儿。
 * 中止请求从事件循环线程进来，读循环在 agent 线程；单 agent 内工具严格顺序执行，
 * 同时至多一个子进程，一个 pid 就记得住，不需要表。
 */
#pragma once

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

/* 工具定义**就是端点要的那个对象**（`name` / `description` / `input_schema`），
 * 外加一个 `_core` 键装 core 私有的字段（`label` / `dangerous`，MCP 来的还有
 * `server` / `remote_name`）。发出去之前 `erase("_core")` 就行（ADR-0023 §2）。
 *
 * 叫 `_core` 不叫 `_meta`：`_meta` 是 MCP 自己的保留字段，server 递来的工具对象里
 * 就可能带着它，另开一个键是为了不把两家的数据搅在一起。 */
nlohmann::json tool_def(std::string name, std::string label, std::string description,
                        std::string_view input_schema, bool dangerous);

/* `_core` 里那两样。**不给默认值**：写 `_core` 的只有两处（`tool_def` 与 `hub.cpp` 拼表），
 * 两处都无条件写齐，缺了就是 core 自己写错了——那时要的是崩，不是多问一句然后继续跑。
 * （「认不出的值按 ask」那条规矩防的是用户手写的配置，不是 core 刚拼好的 JSON。） */
inline bool tool_dangerous(const nlohmann::json &t) { return t["_core"]["dangerous"]; }

/* 归谁执行：空 = 代码在 core 里，非空 = 它跑在那个 server 的进程里。 */
inline std::string tool_server(const nlohmann::json &t)
{
    return t["_core"].value("server", std::string());
}

/* 在一张工具表里按名字找。没有返回 nullptr。 */
inline const nlohmann::json *find_by_name(const nlohmann::json &table, std::string_view name)
{
    for (const nlohmann::json &t : table)
        if (t.value("name", std::string()) == name) return &t;
    return nullptr;
}

/* 内置工具清单（静态表，寿命 = 进程）。**MCP 来的不在这里** ——
 * 那些随外部进程生灭，挂在组上（见 mcp/mcp.hpp 的 McpHub::Lease）。 */
const nlohmann::json &tool_defs();

/* 按名查定义；没有返回 nullptr */
const nlohmann::json *find_tool(std::string_view name);

/* 执行结果就是一个 json：{"content": [块...], "isError": bool}——**MCP 定的那个形状**。
 *
 * 执行。call_id 透传进实时输出帧（tool_output），客户端靠它认领是哪次调用。
 * workdir 是调用方 agent 的工作目录（ADR-0019）：相对路径从这里算起，bash 也 chdir 到这里。
 * core 进程自己的 cwd 与任何 agent 都无关，工具一眼都不该看它。 */
nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const nlohmann::json &params, const EmitFn &emit,
                        const std::string &workdir);

/* 中止在跑的 bash（任意线程）。手上没有在跑的就什么都不做——
 * "下一次调用该不该拒"是 executor 的账，记在这里只会变成一个迟早过期的标志位。 */
void interrupt_tool();

/* —— 结果与参数：每个工具都要的那点东西 —— */

/* 结果只有一种形状：`{"content": [块...], "isError": bool}`。
 *
 * 一个工具不一定只有一段文字好说（读一张图、截一张屏）。**丢东西的地方是知道自己
 * 在丢什么的那一层**——能不能带图片是端点协议的事，`llm/upstream/<协议>.cpp` 知道，
 * 工具不知道。工具照实交出手上的东西，压扁发生在最后一步（ADR-0023 §3）。 */
inline nlohmann::json tool_result(bool is_error, nlohmann::json content)
{
    return nlohmann::json{{"content", std::move(content)}, {"isError", is_error}};
}
/* 一个 text 块。今天内置六个交出来的都是这个。 */
inline nlohmann::json tool_text(bool is_error, std::string text)
{
    return tool_result(is_error,
                       nlohmann::json::array({{{"type", "text"}, {"text", std::move(text)}}}));
}
inline nlohmann::json tool_fail(const std::string &msg) { return tool_text(true, msg); }
inline nlohmann::json tool_ok(const std::string &what) { return tool_text(false, what); }

/* 结果的块数组 → 一段纯文本。**给只收字符串的那些去处用**：两套 OpenAI 协议的工具消息，
 * 以及回放给客户端的 tool_output 帧。非文本块压成一行占位，而不是悄悄丢掉——
 * 模型（和人）知道自己没看见那张图，就不会假装看见了（ADR-0023 §3）。
 *
 * 住在这里而不是 llm/ 里：它是**结果这个形状**的性质，不属于任何一套协议；
 * 而且 session 也要用它，session 不链 llm。 */
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
        // 嵌进来的资源带正文的，那正文就是文本，不是附件
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
        // 说清是什么、多大——这行字的价值就在于它不假装那张图在
        const size_t bytes = b.value("data", std::string()).size() / 4 * 3; // base64 4→3
        out += "[" + b.value("mimeType", type.empty() ? std::string("?") : type) + ", " +
               (bytes >= 1024 ? std::to_string(bytes / 1024) + " KB" : std::to_string(bytes) + " B") +
               " —— 这里带不动]";
    }
    return out;
}

/* 取一个字符串参数；缺失/非字符串返回 nullopt。
 * 参数是模型给的，形状不由 core 说了算——const operator[] 撞上缺键是未定义行为，
 * 这里只能按迭代器查。 */
inline std::optional<std::string> tool_arg(const nlohmann::json &params, std::string_view key)
{
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) return std::nullopt;
    return it->get<std::string>();
}

/* 相对路径从 agent 的工作目录算起（ADR-0019）。core 进程的 cwd 与任何 agent 都无关 */
inline std::string tool_resolve(const std::string &workdir, const std::string &path)
{
    const std::filesystem::path p(path);
    return p.is_absolute() ? path : (std::filesystem::path(workdir) / p).string();
}

/* —— 行与 anchor（ADR-0018）：read 打印它、edit 校验它，是同一份契约 —— */

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

/* 一行的 hash：FNV-1a，3 个十六进制字符。空白不算——跑一遍格式化不该让它变。 */
inline std::string hash_line(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        if (!std::isspace(c)) h = (h ^ c) * 16777619u;
    char buf[4];
    std::snprintf(buf, sizeof buf, "%03x", h & 0xfff);
    return buf;
}

/* —— 每个工具两件事：它的定义、它的实现 —— */

nlohmann::json read_def();
nlohmann::json read_run(const nlohmann::json &params, const std::string &workdir);

nlohmann::json edit_def();
nlohmann::json edit_run(const nlohmann::json &params, const std::string &workdir);

nlohmann::json bash_def();
nlohmann::json bash_run(const std::string &call_id, const nlohmann::json &params,
                        const EmitFn &emit, const std::string &workdir);

nlohmann::json spawn_def();
nlohmann::json send_message_def();

} // namespace realagent
