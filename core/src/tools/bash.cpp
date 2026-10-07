/*
 * bash.cpp — 跑一条 shell 命令，边跑边推输出（tool_output 帧）
 */
#include "proc.hpp"
#include "tools/tools.hpp"

namespace realagent {

nlohmann::json bash_def()
{
    return tool_def(
        "bash", "执行命令",
        "Run a command in the shell; returns stdout and stderr (merged into one stream).\n"
        "Dangerous operations require user confirmation.\n"
        "Do not search text with grep, rg, git grep or the like, alone or inside a longer command:\n"
        "code goes to the search tool; docs, configs, logs and command output go to\n"
        "`realontext query --pattern RE [--path PATH]...`, which also reads a pipe\n"
        "(`git log | realontext query --pattern fix`). Listing files by name stays here (ls, find).",
        R"({"type":"object","properties":{"command":{"type":"string"}},"required":["command"]})",
        true);
}

nlohmann::json bash_run(const std::string &call_id, const nlohmann::json &params,
                        const EmitFn &emit, const std::string &workdir,
                        const std::atomic<bool> *abort)
{
    const auto cmd = tool_arg(params, "command");
    if (!cmd) return tool_fail("missing command");

    ProcSpec spec{.command = *cmd,
                  .cwd = workdir,
                  .merge_stderr = true, // 报错原文是模型判断怎么改的唯一依据
                  .max_out = 50000,
                  .abort = abort};
    if (emit)
        spec.on_line = [&](std::string_view text) {
            emit("tool_output",
                 nlohmann::json{{"call_id", call_id}, {"stream", "output"}, {"text", text}}.dump());
        };

    ProcResult r = run_proc(spec);
    if (r.truncated && r.out.size() >= 3) r.out.replace(r.out.size() - 3, 3, "...");
    return tool_text(r.status != 0, std::move(r.out)); // 非零退出码、被中断都算失败
}

} // namespace realagent
