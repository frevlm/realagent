#include "tools/tools.hpp"

namespace realagent {

namespace {

/* 函数内静态：schema 字面量只 parse 一次。 */
const nlohmann::json &table()
{
    static const nlohmann::json k = nlohmann::json::array(
        {read_def(), search_def(), edit_def(), spawn_def(), send_message_def(), bash_def()});
    return k;
}

} // namespace

nlohmann::json tool_def(std::string name, std::string label, std::string description,
                        std::string_view input_schema, bool dangerous)
{
    nlohmann::json t;
    t["name"] = std::move(name);
    t["description"] = std::move(description);
    t["input_schema"] = nlohmann::json::parse(input_schema); // 编译进来的字面量，解不动就是 bug
    t["_core"] = {{"label", std::move(label)}, {"dangerous", dangerous}};
    return t;
}

const nlohmann::json &tool_defs() { return table(); }

const nlohmann::json *find_tool(std::string_view name) { return find_by_name(table(), name); }

nlohmann::json run_tool(const std::string &call_id, const std::string &name,
                        const nlohmann::json &params, const EmitFn &emit,
                        const std::string &workdir, const std::atomic<bool> *abort)
{
    if (name == "read") return read_run(params, workdir);
    if (name == "search") return search_run(params, workdir, abort);
    if (name == "edit") return edit_run(params, workdir);
    if (name == "bash") return bash_run(call_id, params, emit, workdir, abort);
    return tool_fail("unknown tool: " + name);
}

} // namespace realagent
