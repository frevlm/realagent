/*
 * read.cpp — 按行打出文件，每行前缀 `行号 hash`，正是 edit 的两个入参（ADR-0018）
 */
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json read_def()
{
    return tool_def(
        "read", "读文件",
        "Read a file. Every line starts with `line hash ` — those two values are exactly the\n"
        "line and hash arguments of the edit tool.",
        R"({"type":"object","properties":{"file_path":{"type":"string"}},"required":["file_path"]})",
        false);
}

nlohmann::json read_run(const nlohmann::json &params, const std::string &workdir)
{
    const auto arg_path = tool_arg(params, "file_path");
    if (!arg_path) return tool_fail("missing file_path");
    const std::string path = tool_resolve(workdir, *arg_path);
    if (!std::filesystem::exists(path)) return tool_fail("cannot open: " + path);

    std::string out;
    const auto lines = read_lines(path);
    for (size_t i = 0; i < lines.size(); ++i)
        out += std::to_string(i + 1) + ' ' + hash_line(lines[i]) + ' ' + lines[i] + '\n';
    return tool_ok(std::move(out));
}

} // namespace realagent
