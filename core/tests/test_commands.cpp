/*
 * test_commands.cpp — prompt 命令扫盘与参数展开（ADR-0024 §7）
 *
 * 直接编译 src/agent/commands.cpp + command.cpp + frontmatter.cpp + plugin.cpp，
 * 不起服务、不碰网络。验证：
 *   - plugin 的 commands/<name>.md 扫得到，名字是 `<plugin>:<文件名>`
 *   - 隐式 plugin 的名字不带前缀——既有行为一个字符不变
 *   - 不同名 plugin 的同名命令并存；同名 plugin 的两处，近的覆盖远的
 *   - 内置那三条不可被覆盖：撞上就跳过
 *   - 只取 description / argument-hint / 正文，别的字段一律不收
 *   - ${CLAUDE_PLUGIN_ROOT} 在正文里展开；隐式 plugin 里出现它就是错
 *   - $ARGUMENTS：有占位符就替换（全部），没有就附在末尾（不丢用户打的字）
 *
 * 隔离：HOME 与 workdir 都指到临时目录。
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/catalog.hpp"

namespace fs = std::filesystem;
using realagent::expand_arguments;
using realagent::PromptCommand;
using realagent::scan_commands;

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

/* 装来的 plugin：<root>/.realagent/plugins/<plugin>/commands/<name>.md */
static fs::path put_plugin_cmd(const fs::path &root, const std::string &plugin,
                               const std::string &name, const std::string &body)
{
    const fs::path dir = root / ".realagent" / "plugins" / plugin / "commands";
    fs::create_directories(dir);
    std::ofstream(dir / (name + ".md")) << body;
    return root / ".realagent" / "plugins" / plugin;
}

/* 隐式 plugin：<root>/.realagent/commands/<name>.md */
static void put_cmd(const fs::path &root, const std::string &name, const std::string &body)
{
    const fs::path dir = root / ".realagent" / "commands";
    fs::create_directories(dir);
    std::ofstream(dir / (name + ".md")) << body;
}

static const PromptCommand *find(const std::vector<PromptCommand> &v, const std::string &name)
{
    for (const PromptCommand &c : v)
        if (c.name == name) return &c;
    return nullptr;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path tmp = fs::temp_directory_path() / "realagent_commands_test";
    fs::remove_all(tmp);
    const fs::path home = tmp / "home";
    const fs::path work = tmp / "work";
    fs::create_directories(home);
    fs::create_directories(work);
    setenv("HOME", home.c_str(), 1);

    printf("== 一条都没有 ==\n");
    CHECK(scan_commands(work.string()).empty(), "空清单");

    printf("== plugin 来的：名字带前缀，只取三样 ==\n");
    const fs::path proot = put_plugin_cmd(
        home, "cave", "commit",
        "---\ndescription: 写一条 commit message\nargument-hint: \"[--amend]\"\n"
        "allowed-tools: Bash(rm -rf *)\nmodel: opus\n---\n给暂存的改动写一条 commit message。\n");
    {
        const auto v = scan_commands(work.string());
        const PromptCommand *c = find(v, "cave:commit");
        CHECK(c != nullptr, "名字是 <plugin>:<文件名>");
        CHECK(c && c->description == "写一条 commit message", "description 收");
        CHECK(c && c->argument_hint == "[--amend]", "argument-hint 收");
        CHECK(c && c->body.find("给暂存的改动") != std::string::npos, "正文收");
        // allowed-tools / model 不收：权限与模型档位不该由一段 markdown 决定
        CHECK(c && c->body.find("allowed-tools") == std::string::npos,
              "frontmatter 不混进正文");
    }

    printf("== 隐式 plugin：不带前缀 ==\n");
    put_cmd(home, "mine", "---\ndescription: 我自己写的\n---\n正文\n");
    {
        const auto v = scan_commands(work.string());
        CHECK(find(v, "mine") != nullptr, "既有的名字一个字符都不变");
    }

    printf("== 不同名 plugin 的同名命令并存 ==\n");
    put_plugin_cmd(home, "other", "commit", "---\ndescription: 另一个\n---\n别的正文\n");
    {
        const auto v = scan_commands(work.string());
        CHECK(find(v, "cave:commit") && find(v, "other:commit"), "两条都在，不互相覆盖");
    }

    printf("== 同名 plugin 的两处：近的覆盖远的 ==\n");
    put_plugin_cmd(home, "dup", "one", "---\ndescription: d\n---\n全局那份\n");
    put_plugin_cmd(work, "dup", "one", "---\ndescription: d\n---\n仓库那份\n");
    {
        const auto v = scan_commands(work.string());
        const PromptCommand *c = find(v, "dup:one");
        CHECK(c && c->body.find("仓库那份") != std::string::npos, "workdir 那份赢");
        int n = 0;
        for (const PromptCommand &k : v)
            if (k.name == "dup:one") ++n;
        CHECK(n == 1, "覆盖不是并存");
    }

    printf("== 内置那三条不可被覆盖 ==\n");
    put_cmd(home, "new", "---\ndescription: 想抢 /new\n---\n正文\n");
    put_plugin_cmd(home, "cave", "new", "---\ndescription: 带前缀的 new\n---\n正文\n");
    {
        const auto v = scan_commands(work.string());
        CHECK(find(v, "new") == nullptr, "隐式 plugin 撞上内置名：跳过");
        CHECK(find(v, "cave:new") != nullptr, "带前缀的 new 撞不上，照常收");
    }

    printf("== ${CLAUDE_PLUGIN_ROOT} ==\n");
    put_plugin_cmd(home, "cave", "run",
                   "---\ndescription: r\n---\n跑 ${CLAUDE_PLUGIN_ROOT}/x.js 再跑 "
                   "${CLAUDE_PLUGIN_ROOT}/y.js\n");
    put_cmd(home, "bad", "---\ndescription: b\n---\n${CLAUDE_PLUGIN_ROOT}/x.js\n");
    {
        const auto v = scan_commands(work.string());
        const PromptCommand *c = find(v, "cave:run");
        CHECK(c && c->body.find("${CLAUDE_PLUGIN_ROOT}") == std::string::npos, "展开了");
        CHECK(c && c->body.find((proot / "x.js").string()) != std::string::npos &&
                  c->body.find((proot / "y.js").string()) != std::string::npos,
              "两处都换，不是只换第一处");
        CHECK(find(v, "bad") == nullptr, "隐式 plugin 里出现它就是错，跳过");
    }

    printf("== $ARGUMENTS ==\n");
    CHECK(expand_arguments("查 $ARGUMENTS 再查 $ARGUMENTS", "x") == "查 x 再查 x",
          "有占位符：全部替换");
    CHECK(expand_arguments("正文", "x") == "正文\n\nx", "没占位符：附在末尾，不丢用户打的字");
    CHECK(expand_arguments("正文", "") == "正文", "无参就是正文本身");

    fs::remove_all(tmp);
    printf(failures == 0 ? "\nPASS\n" : "\nFAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
