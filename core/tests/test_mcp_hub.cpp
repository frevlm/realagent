/*
 * test_mcp_hub.cpp — 读配置、合并、连接复用、拼表（ADR-0023 §4）
 *
 * 不碰网络：对手是 mcp_stub_server。验的是：
 *   - 两处来源，同名**整条覆盖**（不是逐字段合并）
 *   - `{"enabled": false}` 在项目级关掉全局那个
 *   - `type` 非 stdio、`${...}` 模板、坏 JSON —— 各自跳过一个，不牵连别的
 *   - **类型上的错由 json 库判**：缺键、值不是字符串、条目不是对象，报的都是库的原话
 *   - **一份配置一个连接**：两个 Lease 拿到的是同一个 McpClient；参数不同才起第二个进程
 *   - 最后一个 Lease 松手 → 连接关掉（池里存的是 weak_ptr，不自己写计数器）
 *   - **归一**：启动规格是一份 JSON，只剩 name / command / args / env，未知键在解析处落下
 *   - 工具表：名字 `<键>__<原名>`、`_core.dangerous` 一律为真、MCP 的 title/annotations 不收
 *
 * 隔离：HOME 与 workdir 都指到临时目录——全局那一处是从 HOME 算出来的。
 */
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "mcp/mcp.hpp"

namespace fs = std::filesystem;
using realagent::load_mcp_config;
using realagent::McpHub;

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

/* 桥接模板：与 config.cpp 默认树里那条一字不差。测试不复述它的语义，只用它。 */
static const nlohmann::json kBridge = {"npx", "-y", "mcp-remote", "{url}",
                                       nlohmann::json::array({"--header", "{name}: {value}"})};

static void write_cfg(const fs::path &root, const std::string &json)
{
    fs::create_directories(root / ".realagent");
    std::ofstream(root / ".realagent" / "mcp.json") << json;
}
/* 一份装来的 plugin 的 MCP 配置：<root>/.realagent/plugins/<name>/.mcp.json
 * 装来的用 Claude Code 的文件名（`.mcp.json`，带点），隐式的用本项目的（`mcp.json`）。 */
static fs::path write_plugin_cfg(const fs::path &root, const std::string &plugin,
                                 const std::string &json)
{
    const fs::path dir = root / ".realagent" / "plugins" / plugin;
    fs::create_directories(dir);
    std::ofstream(dir / ".mcp.json") << json;
    return dir;
}

static void drop_cfg(const fs::path &root)
{
    std::error_code ec;
    fs::remove(root / ".realagent" / "mcp.json", ec);
}
/* 一段指向 stub 的配置。mode 非空时当参数传给 stub。 */
static std::string stub_cfg(const std::string &key, const std::string &mode = "")
{
    std::string args = mode.empty() ? "[]" : "[\"" + mode + "\"]";
    return R"({"mcpServers":{")" + key + R"(":{"command":")" + MCP_STUB_SERVER +
           R"(","args":)" + args + "}}}";
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_home = fs::temp_directory_path() / ("ra_mcp_home_" + std::to_string(getpid()));
    g_work = fs::temp_directory_path() / ("ra_mcp_work_" + std::to_string(getpid()));
    fs::create_directories(g_home);
    fs::create_directories(g_work);
    setenv("HOME", g_home.c_str(), 1);

    printf("== 一个配置都没有 ==\n");
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        CHECK(l.tools.empty() && l.errors.empty(), "空表、无错（文件不存在不是错）");
    }

    printf("\n== 全局一个，连上并拼表 ==\n");
    write_cfg(g_home, stub_cfg("stub"));
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        CHECK(l.errors.empty(), "没有错");
        CHECK(l.tools.size() == 2, "两个工具（分页跟到底了）");
        if (l.tools.size() == 2)
        {
            CHECK(l.tools[0]["name"] == "stub__echo", "名字是 <键>__<原名>");
            CHECK(l.tools[0]["_core"]["dangerous"] == true, "MCP 来的一律 dangerous");
            CHECK(l.tools[0]["_core"]["remote_name"] == "echo", "转发名是原名，不带前缀");
            CHECK(l.tools[0].contains("input_schema"), "schema 换成了端点的键名");
            /* schema 原样转发，`$schema` 也在里面。真 server（官方参考实现）发的就是
             * draft-07，而端点收不收 draft-07 **没有验过**（这台机器上没有凭证）。
             * 钉住现状：哪天决定要剥掉 $schema，这条断言会告诉你改了什么。 */
            CHECK(l.tools[0]["input_schema"].value("$schema", std::string()) ==
                      "http://json-schema.org/draft-07/schema#",
                  "inputSchema 原样转发，$schema 一并带过去（端点收不收未验证）");
            CHECK(!l.tools[0].contains("title") && !l.tools[0].contains("annotations"),
                  "title / annotations 不收（端点没有它们的位置）");
        }
    }

    printf("\n== 项目级同名：整条覆盖，不逐字段合并 ==\n");
    write_cfg(g_work, stub_cfg("stub", "badversion")); // 同名，参数不同
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        CHECK(l.tools.empty(), "近的那条赢了（它连不上，所以没有工具）");
        CHECK(l.errors.size() == 1 && l.errors[0].find("-32022") != std::string::npos,
              "错的是近的那条，不是全局那条");
    }

    printf("\n== 项目级 {\"enabled\": false} 关掉全局那个 ==\n");
    write_cfg(g_work, R"({"mcpServers":{"stub":{"enabled":false}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.empty(), "关掉的不进清单");
        CHECK(cfg.errors.empty(), "关掉的条目不必是一条能跑的规格，没有 command 也不报错");
    }

    printf("\n== 归一：只剩四个键，未知键落下 ==\n");
    write_cfg(g_work,
              R"({"mcpServers":{"stub":{"//":"这是注释","command":"/bin/true","timeout":9,)"
              R"("cwd":"/tmp"}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.size() == 1 && cfg.servers[0].size() == 4,
              "name / command / args / env，多一个都没有");
        CHECK(!cfg.servers[0].contains("timeout"),
              "未知键不进启动规格——那份对象 dump 出来就是连接的键，"
              "留着一个被忽略的 timeout 会让同一个 server 起两个进程");
    }

    printf("\n== 跳过一个不牵连别的 ==\n");
    write_cfg(g_work, R"({"mcpServers":{
        "weird": {"type":"grpc","url":"https://example.com/mcp"},
        "tpl":   {"command":"npx","args":["-y","x","${workspaceFolder}"]}
    }})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.size() == 1 && cfg.servers[0]["name"] == "stub",
              "全局那个 stub 照常在（近处两条都坏，但坏的是它们自己）");
        bool weird = false, tpl = false;
        for (const auto &e : cfg.errors)
        {
            if (e.find("type=\"grpc\"") != std::string::npos) weird = true;
            if (e.find("${workspaceFolder}") != std::string::npos &&
                e.find("绝对路径") != std::string::npos)
                tpl = true;
        }
        CHECK(weird, "不认识的 type：说的是「不认识」，不是拿一个不存在的 command 去 fork");
        CHECK(tpl, "${...}：点名 + 告诉他改成绝对路径，而不是安静地传过去");
    }

    printf("\n== type: http → 归一成一条 stdio 规格（ADR-0024 §5）==\n");
    write_cfg(g_work, R"({"mcpServers":{"docs":{"type":"http","url":"https://x/mcp"}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        const auto *h = &cfg.servers[0];
        for (const auto &c : cfg.servers)
            if (c["name"] == "docs") h = &c;
        CHECK((*h)["command"] == "npx", "命令来自模板，core 一个包名都不认识");
        CHECK((*h)["args"] == nlohmann::json({"-y", "mcp-remote", "https://x/mcp"}),
              "{url} 换掉；一个 header 都没有时，那个子数组整组不出现");
        CHECK(h->size() == 4, "四个键，跟 stdio 那条一模一样——下游不知道它原来是 http");
    }

    printf("\n== header 并进同一个模板，按数量重复 ==\n");
    write_cfg(g_work, R"({"mcpServers":{"docs":{"type":"http","url":"https://x/mcp",
        "headers":{"Authorization":"Bearer t","X-Trace":"1"}}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        std::string all;
        for (const auto &c : cfg.servers)
            if (c["name"] == "docs") all = c["args"].dump();
        CHECK(all.find("--header") != std::string::npos, "header 段出现了");
        CHECK(all.find("Authorization: Bearer t") != std::string::npos &&
                  all.find("X-Trace: 1") != std::string::npos,
              "两个 header 各展开一组，不是只展开第一个");
    }

    printf("\n== 桥接模板空 = 不支持 http，报一条人话 ==\n");
    {
        const auto cfg = load_mcp_config(g_work.string(), nlohmann::json::array());
        bool said = false;
        for (const auto &e : cfg.errors)
            if (e.find("mcp_http_bridge") != std::string::npos) said = true;
        CHECK(said, "点名那个配置键，用户改得动它");
    }

    printf("\n== 坏 JSON：记一条，不拒绝启动，且说清坏在哪 ==\n");
    write_cfg(g_work, "{ this is not json");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.size() == 1, "全局那份照常用");
        CHECK(cfg.errors.size() == 1 && cfg.errors[0].find(g_work.string()) != std::string::npos,
              "错误说的是哪个文件");
        /* 这句是 json 库写的，不是我们拼的：带行号列号和「本来期待什么」。
         * 手写一句「不是合法 JSON」的话，用户还得自己去数括号。 */
        CHECK(cfg.errors[0].find("parse error") != std::string::npos &&
                  cfg.errors[0].find("line 1") != std::string::npos,
              "库的原话：parse error at line 1, column ...");
        printf("  %s\n", cfg.errors[0].c_str());
    }

    printf("\n== 形状不对：库自己指出是哪个键、实际是什么类型 ==\n");
    write_cfg(g_work, R"({"mcpServers":{
        "nocmd": {"args":["x"]},
        "badarg": {"command":"/bin/true","args":[1,2]},
        "notobj": 5,
        "badon": {"enabled":"yes","command":"/bin/true"}
    }})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.size() == 1 && cfg.servers[0]["name"] == "stub",
              "四条都坏，全局那个 stub 照常在");
        std::string all;
        for (const auto &e : cfg.errors) all += e + "\n";
        CHECK(all.find("key 'command' not found") != std::string::npos,
              "缺 command：库点名说是哪个键，不是我们手写的一句「缺 command」");
        // args 里混了数字必须当场爆出来：悄悄丢掉的话，用户拿到一个不带参数的 server
        CHECK(all.find("type must be string, but is number") != std::string::npos,
              "args 里混了数字：不再悄悄丢掉");
        CHECK(all.find("notobj") != std::string::npos, "条目根本不是对象：也接得住，不炸整个 core");
        /* `value("enabled", true)` 撞上字符串会 throw，那条路上必须有 catch：
         * core 是常驻服务，一份手滑的配置不能带走所有 agent。 */
        CHECK(all.find("badon") != std::string::npos,
              "enabled 不是 bool：接住，不是抛穿常驻服务");
        printf("%s", all.c_str());
    }

    printf("\n== 一份配置一个连接 ==\n");
    drop_cfg(g_work);
    {
        McpHub hub;
        auto a = hub.open(g_work.string(), kBridge);
        auto b = hub.open(g_work.string(), kBridge);
        CHECK(a.conns.size() == 1 && b.conns.size() == 1, "各拿到一个");
        CHECK(a.conns[0].client.get() == b.conns[0].client.get(),
              "同一份配置 = 同一个进程，不起第二个");

        std::weak_ptr<realagent::McpClient> watch = a.conns[0].client;
        a.conns.clear();
        CHECK(!watch.expired(), "还有人拿着，连接不关");
        b.conns.clear();
        CHECK(watch.expired(), "最后一个松手，连接自己关（池里是 weak_ptr，没有计数器）");
    }

    printf("\n== 只有名字不同 = 一个进程，两个名字 ==\n");
    write_cfg(g_work, stub_cfg("other")); // 同一条命令，另一个键
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        CHECK(l.conns.size() == 2, "两条租约");
        CHECK(l.conns[0].client.get() == l.conns[1].client.get(),
              "进程由 exec 什么决定，不由它叫什么决定");
        CHECK(l.tools.size() == 4, "工具表按名字各拼一份——共用进程不等于共用名字");
    }

    printf("\n== 参数不同 = 两个进程 ==\n");
    write_cfg(g_work, stub_cfg("other", "slow")); // 另一份规格：多一个参数
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        CHECK(l.conns.size() == 2, "两个连接");
        CHECK(l.conns[0].client.get() != l.conns[1].client.get(), "是两个不同的进程");
        CHECK(l.tools.size() == 4, "两份工具表拼在一起");
        bool has_other = false, has_stub = false;
        for (const auto &t : l.tools)
        {
            if (t["name"] == "other__echo") has_other = true;
            if (t["name"] == "stub__echo") has_stub = true;
        }
        CHECK(has_other && has_stub, "同一个原名，靠前缀分得开");
    }

    printf("\n== 工具名规范化：抄端点第一方客户端那一行 ==\n");
    write_cfg(g_work, stub_cfg("srv", "weirdnames"));
    drop_cfg(g_home);
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        std::vector<std::string> names;
        for (const auto &t : l.tools) names.push_back(t["name"].get<std::string>());
        auto has = [&](const std::string &n) {
            return std::find(names.begin(), names.end(), n) != names.end();
        };
        CHECK(has("srv__greet"), "本来就合法的原样不动");
        CHECK(has("srv__greet__with_Icons_"),
              "空格和括号换成 _（Go SDK 示例里真实存在的名字）");
        CHECK(has("srv__elicit__form_"), "同一条规则，没有第二种写法");
        for (const auto &t : l.tools)
            if (t["name"] == "srv__greet__with_Icons_")
                CHECK(t["_core"]["remote_name"] == "greet (with Icons)",
                      "转发名是**原名**，一个字符没改——规范化只发生在给模型看的那一面");
        CHECK(names.size() == 3, "三个都在");
    }

    printf("\n== plugin 来的 server：名字带前缀，不同 plugin 撞不上（ADR-0024 §3）==\n");
    drop_cfg(g_home);
    drop_cfg(g_work);
    write_plugin_cfg(g_home, "p1", R"({"mcpServers":{"fs":{"command":"/bin/true"}}})");
    write_plugin_cfg(g_home, "p2", R"({"mcpServers":{"fs":{"command":"/bin/true"}}})");
    write_cfg(g_work, R"({"mcpServers":{"fs":{"command":"/bin/false"}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        std::set<std::string> names;
        for (const auto &c : cfg.servers) names.insert(c["name"].get<std::string>());
        CHECK(names.count("p1_fs") && names.count("p2_fs"),
              "两个 plugin 各带一个键为 fs 的 server，是两条独立条目，不是一次覆盖");
        CHECK(names.count("fs"), "隐式 plugin 前缀为空——既有的名字一个字符都不变");
        CHECK(names.size() == 3, "三条，一条不多");
    }

    printf("\n== ${CLAUDE_PLUGIN_ROOT}：只展开这一个 ==\n");
    drop_cfg(g_work);
    fs::remove_all(g_home / ".realagent" / "plugins");
    const fs::path proot = write_plugin_cfg(
        g_home, "p1",
        R"({"mcpServers":{"own":{"command":"node","args":["${CLAUDE_PLUGIN_ROOT}/server.js"]}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.size() == 1 && cfg.errors.empty(), "展开了，没报错");
        CHECK(cfg.servers[0]["args"][0] == (proot / "server.js").string(),
              "展开成这个 plugin 自己的安装路径——作者写不出你机器上的绝对路径");
    }

    printf("\n== 同一个变量在隐式 plugin 里就是错 ==\n");
    fs::remove_all(g_home / ".realagent" / "plugins");
    write_cfg(g_work,
              R"({"mcpServers":{"own":{"command":"node","args":["${CLAUDE_PLUGIN_ROOT}/x.js"]}}})");
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.empty(), "跳过");
        CHECK(cfg.errors.size() == 1 &&
                  cfg.errors[0].find("只在 plugin 里有意义") != std::string::npos,
              "报的是「这儿没有 plugin root」，不是笼统的「不展开变量」");
    }

    printf("\n== 别家的变量照旧不展开 ==\n");
    write_plugin_cfg(g_home, "p1",
                     R"({"mcpServers":{"w":{"command":"npx","args":["${workspaceFolder}"]}}})");
    drop_cfg(g_work);
    {
        const auto cfg = load_mcp_config(g_work.string(), kBridge);
        CHECK(cfg.servers.empty() && cfg.errors.size() == 1,
              "白名单一个名字，不是一套变量系统——plugin 里也一样");
    }

    printf("\n== 一模一样的 server 装在两个 plugin 里：一个进程 ==\n");
    fs::remove_all(g_home / ".realagent" / "plugins");
    write_plugin_cfg(g_home, "a", stub_cfg("s"));
    write_plugin_cfg(g_home, "b", stub_cfg("s"));
    drop_cfg(g_work);
    {
        McpHub hub;
        auto l = hub.open(g_work.string(), kBridge);
        CHECK(l.conns.size() == 2, "两条租约");
        CHECK(l.conns[0].client.get() == l.conns[1].client.get(),
              "指向同一个连接：进程由 exec 什么决定，不由它叫什么决定");
        std::set<std::string> names;
        for (const auto &t : l.tools) names.insert(t["name"].get<std::string>());
        CHECK(names.count("a_s__echo") && names.count("b_s__echo"),
              "工具名照旧靠前缀分开——共用进程不等于共用名字");
    }

    std::error_code ec;
    fs::remove_all(g_home, ec);
    fs::remove_all(g_work, ec);
    printf("\n%s\n", failures == 0 ? "全部通过" : "有失败");
    return failures == 0 ? 0 : 1;
}
