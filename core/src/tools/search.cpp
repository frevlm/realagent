/*
 * search.cpp — 搜代码：调 realontext CLI，回整段函数与类，按问的东西排好序
 *
 * 不走 MCP：只读、不需要权限裁决，也不该随外部进程生灭——跟 read 是一类东西。
 * realontext 随 core 一起发，在 realagent-core 旁边；core 启动时把那个目录排进 PATH 最前。
 * 搜的永远是 agent 的 workdir，所以模型不用（也不能）传目录。
 */
#include "proc.hpp"
#include "tools/tools.hpp"

namespace realagent {

namespace {

/* 单引号包起来交给 /bin/sh：里面只有 ' 本身要转义。 */
std::string sh_quote(const std::string &s)
{
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

} // namespace

nlohmann::json search_def()
{
    return tool_def(
        "search", "搜代码",
        "Search the code in your working directory (uncommitted changes included). Returns whole\n"
        "functions and classes that contain a line matching `pattern`, best first, with line numbers.\n"
        "This is how you find code: use it instead of grep or rg, at every step of the task, not only\n"
        "the first: to check a name, to follow a call to the next function, to list every caller.\n"
        "\n"
        "Use it for:\n"
        "  - where or how a behavior is implemented, before changing code you have not read\n"
        "  - where a name is defined, what calls it, where it is used\n"
        "  - which line of a file a name is on\n"
        "Not for:\n"
        "  - docs, configs, logs, a command's output: run `realontext query --pattern RE\n"
        "    [--path PATH]...` with bash, or pipe into it: `git log | realontext query --pattern fix`\n"
        "  - a file whose path you know: read it\n"
        "\n"
        "Up to 20 results: the first three as full source, the rest as one header line each plus the\n"
        "lines that matched; a match outside code as `path:line: text`. Nothing back: loosen the\n"
        "pattern. Too many: narrow it.\n"
        "\n"
        "Examples:\n"
        "  search(pattern: \"truncate_name\\(\")  every call of a known name\n"
        "  search(pattern: \"(?i)retry|backoff|attempt\", query: \"where a failed request is retried\n"
        "         with backoff\")  a behavior",
        R"({"type":"object","properties":{
            "pattern":{"type":"string","description":"The grep you would have run: a ripgrep regex, matched line by line, case sensitive unless it starts with (?i). Only code with a matching line is searched, so this decides what can be found. A known name: write it exactly (`truncate_name\\(` for its calls). A behavior: the keywords the code must contain, as a broad case-insensitive alternation (`(?i)retry|backoff|attempt`)."},
            "query":{"type":"string","description":"Only when describing a behavior: one short English sentence with the words likely to appear in the code: domain terms, parts of identifiers, error or log text. Translate a non-English request and add synonyms. Not a regex. It reranks the results, in a few seconds. Leave it out for a known name, its definition, callers or uses."}},
            "required":["pattern"]})",
        false);
}

nlohmann::json search_run(const nlohmann::json &params, const std::string &workdir,
                          const std::atomic<bool> *abort)
{
    const auto pattern = tool_arg(params, "pattern");
    if (!pattern) return tool_fail("missing pattern");

    // --path 必须给：不给的话它看见 stdin 是管道，就去搜 stdin 了
    std::string cmd = "realontext query --path . --pattern " + sh_quote(*pattern);
    if (const auto query = tool_arg(params, "query"); query && !query->empty())
        cmd += " --query " + sh_quote(*query);

    ProcResult r = run_proc(
        {.command = cmd, .cwd = workdir, .timeout_ms = 60000, .max_out = 50000, .abort = abort});
    if (r.truncated && r.out.size() >= 3) r.out.replace(r.out.size() - 3, 3, "...");

    if (r.status == 0) return tool_ok(std::move(r.out));
    if (r.status == 1) return tool_ok("no match: no line matched the pattern, loosen it");
    if (r.status == 127) return tool_fail("realontext not found: it ships next to realagent-core; rebuild core, or npm install -g realontext");
    return tool_fail(r.fail.empty() ? r.err : r.fail);
}

} // namespace realagent
