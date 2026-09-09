/*
 * tools.cpp — 工具壳：一张表 + 按名派发
 *
 * 每个工具的定义与实现各占一个文件（read.cpp / edit.cpp / bash.cpp /
 * spawn.cpp / send_message.cpp），这里只把它们串起来。
 * 加一个工具 = 加一个文件 + 这里两行；改一个工具，壳一个字都不用动。
 */
#include "tools/tools.hpp"

namespace realagent {

namespace {

/* 顺序即模型看到的顺序。函数内静态：跨翻译单元的初始化顺序在这里不构成问题。
 * 五个字面量的 JSON Schema **在这里 parse 一次**，不是每次 build_dialog 都来一遍。 */
const nlohmann::json &table()
{
    static const nlohmann::json k = nlohmann::json::array(
        {read_def(), edit_def(), spawn_def(), send_message_def(), bash_def()});
    return k;
}

} // namespace

nlohmann::json tool_def(std::string name, std::string label, std::string description,
                        std::string_view input_schema, bool dangerous)
{
    nlohmann::json t;
    t["name"] = std::move(name);
    t["description"] = std::move(description);
    /* schema 是编译进来的字面量，解不动就是 core 自己写错了——不兜底 */
    t["input_schema"] = nlohmann::json::parse(input_schema);
    t["_core"] = {{"label", std::move(label)}, {"dangerous", dangerous}};
    return t;
}

const nlohmann::json &tool_defs() { return table(); }

const nlohmann::json *find_tool(std::string_view name) { return find_by_name(table(), name); }

nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const nlohmann::json &params, const EmitFn &emit,
                        const std::string &workdir)
{
    if (name == "read") return read_run(params, workdir);
    if (name == "edit") return edit_run(params, workdir);
    if (name == "bash") return bash_run(call_id, params, emit, workdir);
    // spawn / send_message 由 Executor 拦下，走不到这里
    return tool_fail("unknown tool: " + name);
}

} // namespace realagent
