/*
 * test_plugin_e2e.cpp — 一个 plugin 目录，五样成员一次吃下（ADR-0024）
 *
 * 这份测试的判据就是 ADR 的成败标准：**现存的 Claude Code plugin 能不能 clone 下来直接吃**。
 * 于是这里造的目录逐字照抄那个布局——`.claude-plugin/plugin.json`、`skills/<name>/SKILL.md`、
 * `commands/<name>.md`、`agents/<name>.md`、`hooks/hooks.json`、`.mcp.json`——
 * 一个字段都不迁就本项目。
 *
 * 隔离：HOME 与 workdir 都指到临时目录。
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "agent/agent_defs.hpp"
#include "agent/commands.hpp"
#include "agent/hooks.hpp"
#include "agent/skills.hpp"
#include "mcp/mcp.hpp"
#include "plugin.hpp"

namespace fs = std::filesystem;
using namespace realagent;

static int failures = 0;
#define CHECK(cond, msg)                 \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            printf("  ok: %s\n", msg);   \
        }                                \
        else                             \
        {                                \
            printf("  FAIL: %s\n", msg); \
            ++failures;                  \
        }                                \
    } while (0)

static void put(const fs::path &p, const std::string &text)
{
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path tmp = fs::temp_directory_path() / "realagent_plugin_e2e";
    fs::remove_all(tmp);
    const fs::path home = tmp / "home", work = tmp / "work";
    fs::create_directories(home);
    fs::create_directories(work);
    setenv("HOME", home.c_str(), 1);

    // —— 一个照抄 Claude Code 布局的 plugin ——
    const fs::path p = home / ".realagent" / "plugins" / "demo";
    put(p / ".claude-plugin" / "plugin.json",
        R"({"name":"demo","version":"1.2.3","description":"演示用"})");
    put(p / "skills" / "review" / "SKILL.md", "---\ndescription: >\n  折叠标量写的描述，\n"
                                              "  它跨了两行。\n---\n正文\n");
    put(p / "commands" / "commit.md", "---\ndescription: 写 commit\nargument-hint: \"[--amend]\"\n"
                                      "---\n跑 ${CLAUDE_PLUGIN_ROOT}/bin/x $ARGUMENTS\n");
    // name 故意与文件名不一致：名字取文件名，与 skill 取目录名、command 取文件名同一条规则
    put(p / "agents" / "builder.md", "---\nname: something-else\ndescription: 只改一两个文件\n"
                                     "tools: [Read, Edit]\n---\nCaveman. 一个文件最好。\n");
    put(p / "hooks" / "hooks.json",
        R"({"hooks":{"SessionStart":[{"matcher":"startup","hooks":[
           {"type":"command","command":"echo 来自 ${CLAUDE_PLUGIN_ROOT}"}]}]}})");
    put(p / ".mcp.json", R"({"mcpServers":{
        "docs": {"type":"http","url":"https://docs.example.com/mcp"},
        "own":  {"command":"node","args":["${CLAUDE_PLUGIN_ROOT}/server.js"]}}})");

    const std::string w = work.string();
    const nlohmann::json bridge = {"npx", "-y", "mcp-remote", "{url}",
                                   nlohmann::json::array({"--header", "{name}: {value}"})};

    printf("== skill ==\n");
    {
        const auto v = scan_skills(w);
        CHECK(v.size() == 1 && v[0].name == "demo:review", "名字带 plugin 前缀");
        CHECK(v[0].description.find("它跨了两行") != std::string::npos,
              "YAML 折叠标量整句拿到——这正是不手写 frontmatter 解析器的理由");
    }

    printf("== command ==\n");
    {
        const auto v = scan_commands(w);
        CHECK(v.size() == 1 && v[0].name == "demo:commit", "名字带前缀");
        CHECK(v[0].argument_hint == "[--amend]", "argument-hint 收");
        CHECK(v[0].body.find((p / "bin" / "x").string()) != std::string::npos,
              "${CLAUDE_PLUGIN_ROOT} 在正文里展开");
        CHECK(expand_arguments(v[0].body, "-n").find("-n") != std::string::npos, "$ARGUMENTS 展开");
    }

    printf("== agent 定义 ==\n");
    {
        const auto v = scan_agent_defs(w);
        CHECK(v.size() == 1 && v[0].name == "demo:builder",
              "名字带前缀，且取的是文件名——frontmatter 里那个 name 不作数");
        CHECK(v[0].body.find("Caveman") != std::string::npos, "正文原样，接在 system prompt 后面");
        CHECK(!agent_defs_prompt(v).empty() && agent_defs_prompt({}).empty(),
              "一个都没有时提示词是空串");
    }

    printf("== hook ==\n");
    {
        const Hooks h = Hooks::scan(w);
        CHECK(!h.empty() && h.errors().empty(), "读到了，没报错");
        const HookOutcome o = h.run(HookEvent::SessionStart, {{"matcher_key", "startup"}});
        CHECK(o.inject.find(p.string()) != std::string::npos,
              "matcher 命中，${CLAUDE_PLUGIN_ROOT} 在命令里展开");
        CHECK(h.run(HookEvent::SessionStart, {{"matcher_key", "resume"}}).inject.empty(),
              "matcher 不命中就不跑");
    }

    printf("== MCP：http 与 stdio 归一成同一种东西 ==\n");
    {
        const auto cfg = load_mcp_config(w, bridge);
        CHECK(cfg.errors.empty(), "两条都没报错");
        CHECK(cfg.servers.size() == 2, "两条都在");
        const nlohmann::json *docs = nullptr, *own = nullptr;
        for (const auto &c : cfg.servers)
        {
            if (c["name"] == "demo_docs") docs = &c;
            if (c["name"] == "demo_own") own = &c;
        }
        CHECK(docs && (*docs)["command"] == "npx", "http 变成一条桥接命令");
        CHECK(docs && (*docs)["args"][2] == "https://docs.example.com/mcp", "{url} 换掉了");
        CHECK(docs && docs->size() == 4 && own && own->size() == 4,
              "两条形状一模一样——下游不知道哪条原来是 http");
        CHECK(own && (*own)["args"][0] == (p / "server.js").string(),
              "stdio 那条的 ${CLAUDE_PLUGIN_ROOT} 也展开了");
    }

    printf("== /plugins 那张表 ==\n");
    {
        const auto v = plugin_infos(w);
        const PluginInfo *d = nullptr;
        for (const PluginInfo &x : v)
            if (x.name == "demo") d = &x;
        CHECK(d != nullptr && !d->implicit, "装来的，不是隐式");
        CHECK(d && d->version == "1.2.3" && d->description == "演示用", "名片读到了");
        CHECK(d && d->skills == 1 && d->commands == 1 && d->agent_defs == 1 &&
                  d->mcp_servers == 2 && d->hooks == 1,
              "五样都数对了");
    }

    printf("== 隐式 plugin 的名字一个字符都没变 ==\n");
    {
        put(home / ".realagent" / "skills" / "mine" / "SKILL.md", "---\ndescription: 我的\n---\n");
        put(home / ".realagent" / "mcp.json",
            R"({"mcpServers":{"fs":{"command":"/bin/true"}}})");
        const auto sk = scan_skills(w);
        bool bare = false;
        for (const Skill &s : sk)
            if (s.name == "mine") bare = true;
        CHECK(bare, "skill 还是 `mine`，不是 `user:mine`");
        bool bare_mcp = false;
        for (const auto &c : load_mcp_config(w, bridge).servers)
            if (c["name"] == "fs") bare_mcp = true;
        CHECK(bare_mcp, "MCP server 还是 `fs`，工具名还是 `fs__*`");
    }

    fs::remove_all(tmp);
    printf(failures == 0 ? "\nPASS\n" : "\nFAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
