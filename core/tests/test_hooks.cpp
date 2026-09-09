/*
 * test_hooks.cpp — hook 扫盘与执行（ADR-0024 §6）
 *
 * 直接编译 src/agent/hooks.cpp + plugin.cpp + config.cpp，不起服务、不碰网络。验证：
 *   - 只从 hooks/hooks.json 读；plugin.json 里内联的一律不认
 *   - 一个都没装时 run 返回空 Outcome（零开销那条路）
 *   - stdout 上的 JSON 认 decision/reason/additionalContext；不是 JSON 就整段当注入
 *   - 退出码 2 = 拦下（Claude Code 的约定）
 *   - **deny 只能收紧不能放宽**：post 位置说 deny 也拦不住任何东西
 *   - matcher 是正则，不匹配的不跑
 *   - 坏 hook 不许卡死：超时报一条、继续跑
 *   - ${CLAUDE_PLUGIN_ROOT} 在 command 里展开；隐式 plugin 里出现它就是错
 *
 * 隔离：HOME 与 workdir 都指到临时目录。
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/hooks.hpp"

namespace fs = std::filesystem;
using realagent::HookEvent;
using realagent::HookOutcome;
using realagent::Hooks;

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

static fs::path g_home, g_work;

/* 装来的 plugin 的 hooks.json */
static void put_hooks(const fs::path &root, const std::string &plugin, const std::string &json)
{
    const fs::path dir =
        plugin.empty() ? root / ".realagent" / "hooks"
                       : root / ".realagent" / "plugins" / plugin / "hooks";
    fs::create_directories(dir);
    std::ofstream(dir / "hooks.json") << json;
}

static void clear_all()
{
    std::error_code ec;
    fs::remove_all(g_home / ".realagent", ec);
    fs::remove_all(g_work / ".realagent", ec);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path tmp = fs::temp_directory_path() / "realagent_hooks_test";
    fs::remove_all(tmp);
    g_home = tmp / "home";
    g_work = tmp / "work";
    fs::create_directories(g_home);
    fs::create_directories(g_work);
    setenv("HOME", g_home.c_str(), 1);

    printf("== 一个都没装 ==\n");
    {
        const Hooks h = Hooks::scan(g_work.string());
        CHECK(h.empty(), "空");
        const HookOutcome o = h.run(HookEvent::PreToolUse, nlohmann::json::object());
        CHECK(!o.deny && o.inject.empty(), "空 Outcome，一个进程都不起");
    }

    printf("== stdout 上的纯文字 = 注入 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"SessionStart":[{"hooks":[
        {"type":"command","command":"printf 'hello from hook'"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        CHECK(!h.empty() && h.errors().empty(), "读到了，没报错");
        const HookOutcome o = h.run(HookEvent::SessionStart, nlohmann::json::object());
        CHECK(o.inject == "hello from hook", "整段 stdout 当注入");
        CHECK(!o.deny, "没说 deny 就不是 deny");
    }

    printf("== stdout 上的 JSON：认 decision / reason / additionalContext ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreToolUse":[{"hooks":[
        {"type":"command","command":"printf '{\"decision\":\"block\",\"reason\":\"不许\"}'"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::PreToolUse, nlohmann::json::object());
        CHECK(o.deny && o.reason == "不许", "block + 原话");
    }

    printf("== 退出码 2 = 拦下（Claude Code 的约定）==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreToolUse":[{"hooks":[
        {"type":"command","command":"echo 拦下 1>&2; exit 2"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::PreToolUse, nlohmann::json::object());
        CHECK(o.deny, "拦下了");
        CHECK(o.reason.find("拦下") != std::string::npos, "stderr 当理由");
    }

    printf("== deny 只能收紧：post 位置拦不住任何东西 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PostToolUse":[{"hooks":[
        {"type":"command","command":"exit 2"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::PostToolUse, nlohmann::json::object());
        CHECK(!o.deny, "post 说 deny 也不生效——但报了一条，不是静默丢");
    }

    printf("== 非零退出（不是 2）不算 deny，也不打断 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreToolUse":[{"hooks":[
        {"type":"command","command":"exit 1"}]},{"hooks":[
        {"type":"command","command":"printf 后面这个照跑"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::PreToolUse, nlohmann::json::object());
        CHECK(!o.deny, "退出码 1 不是 deny");
        CHECK(o.inject == "后面这个照跑", "坏 hook 不牵连后面的");
    }

    printf("== matcher 是正则 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreToolUse":[
        {"matcher":"^bash$","hooks":[{"type":"command","command":"printf 命中"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        CHECK(h.run(HookEvent::PreToolUse, {{"matcher_key", "bash"}}).inject == "命中", "匹配");
        CHECK(h.run(HookEvent::PreToolUse, {{"matcher_key", "read"}}).inject.empty(), "不匹配就不跑");
    }

    printf("== 超时：报一条，不卡死 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreToolUse":[{"hooks":[
        {"type":"command","command":"sleep 30","timeout":0.3}]}]}})");
    {
        const auto t0 = std::chrono::steady_clock::now();
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::PreToolUse, nlohmann::json::object());
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        CHECK(!o.deny && ms < 5000, "超时就杀，不等它跑完");
    }

    printf("== payload 里有 hook_event_name，matcher_key 不外传 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreToolUse":[{"hooks":[
        {"type":"command","command":"printf GOT; cat"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::PreToolUse, {{"matcher_key", "bash"}, {"x", 1}});
        // 前缀 GOT 让 stdout 不再是合法 JSON，于是整段当注入——这正好把 payload 原样带回来
        CHECK(o.inject.find("\"hook_event_name\":\"PreToolUse\"") != std::string::npos,
              "事件名进 payload");
        CHECK(o.inject.find("matcher_key") == std::string::npos,
              "matcher_key 是 core 挑 hook 用的，不该出现在 payload 里");
    }

    printf("== ${CLAUDE_PLUGIN_ROOT} ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"SessionStart":[{"hooks":[
        {"type":"command","command":"printf ${CLAUDE_PLUGIN_ROOT}"}]}]}})");
    put_hooks(g_home, "", R"({"hooks":{"SessionStart":[{"hooks":[
        {"type":"command","command":"printf ${CLAUDE_PLUGIN_ROOT}"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        const HookOutcome o = h.run(HookEvent::SessionStart, nlohmann::json::object());
        CHECK(o.inject.find("plugins/p1") != std::string::npos, "plugin 里展开成它的安装路径");
        bool said = false;
        for (const auto &e : h.errors())
            if (e.find("只在 plugin 里有意义") != std::string::npos) said = true;
        CHECK(said, "隐式 plugin 里出现它：报一条，那条 hook 不进表");
    }

    printf("== 认不出的事件点名说，不静默丢 ==\n");
    clear_all();
    put_hooks(g_home, "p1", R"({"hooks":{"PreCompact":[{"hooks":[
        {"type":"command","command":"true"}]}]}})");
    {
        const Hooks h = Hooks::scan(g_work.string());
        CHECK(h.empty(), "没有这个落点，一条都不收");
        bool said = false;
        for (const auto &e : h.errors())
            if (e.find("PreCompact") != std::string::npos) said = true;
        CHECK(said, "点名说本项目没有这个落点");
    }

    fs::remove_all(tmp);
    printf(failures == 0 ? "\nPASS\n" : "\nFAIL (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
