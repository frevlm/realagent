/*
 * test_tools.cpp — 内置工具与权限裁决单元测试
 *
 * 直接编译 src/tools/tools.cpp + src/agent/executor.cpp + src/agent/approval.cpp，
 * 不起服务、不碰网络。这里守的是 ADR-0016 真正换掉的那半边：
 * 从前 read/edit/bash 是一个 C 容器、两种权限是两个动态库，现在是静态表 + 一个 switch。
 *
 * 验证：
 *   - 工具清单：三个工具、危险位、查不到的名字返回 nullptr
 *   - read：带 anchor 的输出、分页、输出上限、超大文件（ADR-0018）
 *   - edit：一个操作四种用法 / 先全校验再写 / 陈旧 anchor 拒绝 / 多文件遇错即停
 *   - bash：stdout 回传、非零退出码即错误、缺参数
 *   - 权限（ADR-0005 / ADR-0016）：allow-all 放行、deny 拒绝、ask 真等裁决、
 *     认不出的值按 ask 处理（写错配置该多问一句，不该多放一次行）
 *   - 中止（ADR-0002 R8）：执行前的中止标记撞得上，跑着的 bash 打得断，reset 抹得掉
 *
 * 隔离：HOME 指到临时目录（Config 只认 ~/.realagent/settings.json），
 * 文件操作全在临时目录里，cwd 也指过去。
 */
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "agent/approval.hpp"
#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "config.hpp"
#include "mcp/mcp.hpp"
#include "tools/tools.hpp"

namespace fs = std::filesystem;
using namespace realagent;
using nlohmann::json;

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

static fs::path g_home;

static void write_settings(const std::string &body)
{
    std::ofstream f(g_home / ".realagent" / "settings.json");
    f << body;
}

/* 按 permission 值造一份配置（其余键取默认） */
static Config config_with(const std::string &permission)
{
    write_settings(R"({"permission":")" + permission + R"("})");
    auto c = Config::load();
    if (!c)
    {
        printf("  FAIL: 配置加载失败 %s\n", c.error().c_str());
        ++failures;
        std::exit(1);
    }
    return *c;
}

static std::string slurp(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static json call(const std::string &name, const std::string &params)
{
    // workdir 取 g_home：测试里路径都是绝对的，这个值只有 bash 的 chdir 会用到
    return run_tool("call-1", name, json::parse(params), nullptr, g_home.string());
}

/* 结果就是 MCP 那个形状：{"content": [块...], "isError"}（executor 再加 "interrupted"）。
 * st() 保留「非零即错」的读法，测试里那几十处判断因此一个字都不用改。 */
static int st(const json &r) { return r.value("isError", true) ? 1 : 0; }
static std::string msg(const json &r)
{
    std::string s;
    for (const auto &b : r.value("content", json::array()))
        if (b.value("type", std::string()) == "text") s += b.value("text", std::string());
    return s;
}
static bool intr(const json &r) { return r.value("interrupted", false); }

/* 参数里的路径要进 JSON 字符串，临时目录路径里不含需要转义的字符 */
static std::string q(const fs::path &p) { return "\"" + p.string() + "\""; }

/* read 输出里第 n 行的 hash（行格式 `行号 hash 正文`）。取不到返回空串。
 * 测试走的路跟模型一样：read 印什么就原样填给 edit。 */
static std::string hash_at(const std::string &out, size_t n)
{
    const std::string pre = std::to_string(n) + " ";
    for (size_t p = 0; p < out.size();)
    {
        if (out.compare(p, pre.size(), pre) == 0) return out.substr(p + pre.size(), 3);
        const size_t eol = out.find('\n', p);
        if (eol == std::string::npos) break;
        p = eol + 1;
    }
    return {};
}

/* `{e}` 交给 json 的初始化列表会被折成对象本身而不是单元素数组，所以显式收 list */
static std::string edits(std::initializer_list<json> es)
{
    json arr = json::array();
    for (const json &e : es) arr.push_back(e);
    return json{{"edits", arr}}.dump();
}

int main()
{
    g_home = fs::temp_directory_path() / ("realagent-tools-test-" + std::to_string(::getpid()));
    fs::remove_all(g_home);
    fs::create_directories(g_home / ".realagent");
    ::setenv("HOME", g_home.c_str(), 1);
    fs::current_path(g_home);

    printf("== 工具清单 ==\n");
    {
        // 一个工具一个文件，read/edit/bash 自带实现，spawn/send_message 由 Executor 实现——
        // 定义都在同一张表里，LLM 看见的清单只有一份（ADR-0019）
        CHECK(tool_defs().size() == 5, "五个工具");
        CHECK(find_tool("spawn") && find_tool("send_message"), "两个 agent 级工具在同一张表里");
        const nlohmann::json *r = find_tool("read");
        const nlohmann::json *e = find_tool("edit");
        const nlohmann::json *b = find_tool("bash");
        CHECK(r && !tool_dangerous(*r), "read 是只读工具，不触发权限检查点");
        CHECK(e && tool_dangerous(*e), "edit 危险");
        CHECK(b && tool_dangerous(*b), "bash 危险");
        // 出口不再是一个工具：收工归小模型判（ADR-0025），模型的清单里没有 stop
        CHECK(find_tool("stop") == nullptr, "没有 stop 工具");
        // 定义就是端点要的那个对象，外加一个 core 私有的键（ADR-0023 §2）
        CHECK(r && r->contains("input_schema") && (*r)["input_schema"].is_object(),
              "schema 是对象，不是待 parse 的字符串");
        CHECK(r && r->contains("_core") && (*r)["_core"].contains("label"),
              "core 私有的字段收在 _core 里，发出去之前抹掉那一个键即可");
        CHECK(find_tool("core-tools_bash") == nullptr,
              "没有命名空间前缀这回事了（ADR-0016）");
        CHECK(st(call("nope", "{}")) != 0, "未知工具返回错误，不是空成功");
    }

    printf("== read ==\n");
    {
        const fs::path f = g_home / "r.txt";
        {
            std::ofstream o(f);
            o << "hello\nworld\n";
        }
        const auto r = call("read", R"({"file_path":)" + q(f) + "}");
        CHECK(st(r) == 0, "读得到");
        const std::string h1 = hash_at(msg(r), 1), h2 = hash_at(msg(r), 2);
        CHECK(h1.size() == 3 && h2.size() == 3 && h1 != h2, "每行一个 3 字符 hash");
        CHECK(msg(r) == "1 " + h1 + " hello\n2 " + h2 + " world\n", "格式是 `行号 hash 正文`");

        const auto missing = call("read", R"({"file_path":)" + q(g_home / "nope.txt") + "}");
        CHECK(st(missing) != 0, "文件不存在 → 报错，不是空内容");
        const auto noarg = call("read", "{}");
        CHECK(st(noarg) != 0 && msg(noarg).find("file_path") != std::string::npos,
              "缺 file_path → 报错点名缺的是哪个");
    }

    printf("== edit ==\n");
    {
        const fs::path f = g_home / "e.txt";
        const auto reread = [&f] { return msg(call("read", R"({"file_path":)" + q(f) + "}")); };
        const auto one = [&f](int line, const std::string &h, const std::string &t) {
            json e{{"file_path", f.string()}, {"new_text", t}};
            if (line)
            {
                e["line"] = line;
                e["hash"] = h;
            }
            return edits({e});
        };

        fs::remove(f);
        CHECK(st(call("edit", one(0, "", "a\nb\nc"))) == 0 && slurp(f) == "a\nb\nc\n",
              "不给 line = 写整个文件（创建）");

        std::string out = reread();
        CHECK(st(call("edit", one(2, hash_at(out, 2), "B"))) == 0 && slurp(f) == "a\nB\nc\n",
              "换掉一行");

        out = reread();
        CHECK(st(call("edit", one(1, hash_at(out, 1), "a\nNEW"))) == 0 &&
                  slurp(f) == "a\nNEW\nB\nc\n",
              "new_text 带换行即换成多行");

        out = reread();
        CHECK(st(call("edit", one(2, hash_at(out, 2), ""))) == 0 && slurp(f) == "a\nB\nc\n",
              "空串即删掉这一行");

        out = reread();
        CHECK(st(call("edit", one(2, "___", "X"))) != 0 && slurp(f) == "a\nB\nc\n",
              "hash 对不上 → 拒绝，文件原样");
        CHECK(st(call("edit", one(99, hash_at(out, 1), "X"))) != 0, "行号越界 → 拒绝");
    }

    printf("== edit：多条、跨文件、遇错即停 ==\n");
    {
        const fs::path a = g_home / "m1.txt", b = g_home / "m2.txt";
        for (const auto &p : {a, b})
        {
            std::ofstream o(p);
            o << "x\ny\n";
        }
        const auto ra = msg(call("read", R"({"file_path":)" + q(a) + "}"));
        const auto rb = msg(call("read", R"({"file_path":)" + q(b) + "}"));

        // 两条都改第 1 行：单行 hash 不含邻域，所以互不影响
        CHECK(st(call("edit", edits({{{"file_path", a.string()}, {"line", 1}, {"hash", hash_at(ra, 1)}, {"new_text", "A"}},
                                     {{"file_path", b.string()}, {"line", 1}, {"hash", hash_at(rb, 1)}, {"new_text", "B"}}}))) == 0 &&
                  slurp(a) == "A\ny\n" && slurp(b) == "B\ny\n",
              "一次调用跨两个文件");

        // 同一个文件的相邻两行：单行 hash 不含邻域，改了上一行也不影响下一行
        const auto ra2 = msg(call("read", R"({"file_path":)" + q(a) + "}"));
        CHECK(st(call("edit", edits({{{"file_path", a.string()}, {"line", 1}, {"hash", hash_at(ra2, 1)}, {"new_text", "1"}},
                                     {{"file_path", a.string()}, {"line", 2}, {"hash", hash_at(ra2, 2)}, {"new_text", "2"}}}))) == 0 &&
                  slurp(a) == "1\n2\n",
              "相邻两行同批改——hash 只看本行，改上一行不作废下一行");

        // 第二条坏掉：第一条已经落盘，报错要说清停在第几条
        const auto ra3 = msg(call("read", R"({"file_path":)" + q(a) + "}"));
        const auto partial = call("edit", edits({{{"file_path", a.string()}, {"line", 1}, {"hash", hash_at(ra3, 1)}, {"new_text", "OK"}},
                                                 {{"file_path", b.string()}, {"line", 1}, {"hash", "___"}, {"new_text", "NO"}}}));
        CHECK(st(partial) != 0 && msg(partial).find("edit 2") != std::string::npos,
              "遇错即停，报错点名第几条");
        CHECK(slurp(a) == "OK\n2\n" && slurp(b) == "B\ny\n", "前一条已写入，出错那条没动");
        CHECK(msg(partial).find("1 edit already written") != std::string::npos, "说清前面写了几条");

        CHECK(st(call("edit", "{}")) != 0, "缺 edits → 报错");
    }

    printf("== workdir：相对路径与 bash 的 cwd 都跟着它（ADR-0019） ==\n");
    {
        // core 进程的 cwd 与 agent 的 workdir 故意指到不同地方——多 agent 之后
        // 前者是"启动 core 那个 shell 当时在哪"，跟任何 agent 都无关
        const fs::path wd = g_home / "wd";
        fs::create_directories(wd);
        {
            std::ofstream o(wd / "inside.txt");
            o << "here\n";
        }
        fs::current_path(g_home); // 进程 cwd 停在别处

        CoreContext ctx{.config = nullptr, .emit_fn = nullptr};
        ApprovalCoordinator ap;
        Executor exe(ctx, ap, wd.string());

        const auto rel = run_tool("w1", "read", json::parse(R"({"file_path":"inside.txt"})"), nullptr, wd.string());
        CHECK(st(rel) == 0 && msg(rel).find("here") != std::string::npos,
              "相对路径从 workdir 算起，不是从进程 cwd");
        const auto miss =
            run_tool("w2", "read", json::parse(R"({"file_path":"inside.txt"})"), nullptr, g_home.string());
        CHECK(st(miss) != 0, "换个 workdir 同一条相对路径就找不到——说明真的按它解析");

        const auto abs =
            run_tool("w3", "read", json{{"file_path", (wd / "inside.txt").string()}}, nullptr,
                     g_home.string());
        CHECK(st(abs) == 0, "绝对路径不受 workdir 影响");

        const auto pwd = run_tool("w4", "bash", json::parse(R"({"command":"pwd"})"), nullptr, wd.string());
        CHECK(st(pwd) == 0 && msg(pwd).find(wd.string()) != std::string::npos,
              "bash 起来时 chdir 到 workdir，不继承 core 进程的 cwd");

        const auto created =
            run_tool("w5", "edit",
                     json::parse(R"({"edits":[{"file_path":"made.txt","new_text":"x"}]})"), nullptr,
                     wd.string());
        CHECK(st(created) == 0 && fs::exists(wd / "made.txt"), "edit 的相对路径同样按 workdir 落地");
    }

    printf("== bash ==\n");
    {
        const auto ok = call("bash", R"({"command":"echo tools-ok"})");
        CHECK(st(ok) == 0 && msg(ok) == "tools-ok\n", "stdout 原样回传");

        const auto rc = call("bash", R"({"command":"exit 3"})");
        CHECK(st(rc) != 0, "非零退出码即错误（结果是 isError 布尔，不带退出码）");

        const auto noarg = call("bash", "{}");
        CHECK(st(noarg) != 0 && msg(noarg).find("command") != std::string::npos,
              "缺 command → 报错");

        // B1（ADR-0017）：只接 stdout 的话，失败命令回给模型的是"退出码非零，无话可说"
        const auto err = call("bash", R"({"command":"echo BOOM >&2; exit 7"})");
        CHECK(st(err) != 0, "非零退出码即错误");
        CHECK(msg(err).find("BOOM") != std::string::npos,
              "stderr 也回传——报错原文是模型判断该不该重试的唯一依据");

        const auto both = call("bash", R"({"command":"echo one; echo two >&2; echo three"})");
        CHECK(msg(both) == "one\ntwo\nthree\n",
              "两条流合流，顺序就是人在终端里看见的顺序");
    }

    printf("== 权限裁决（一个配置键，一个 switch） ==\n");
    {
        // allow-all：危险工具直接跑，没有任何问询
        Config cfg = config_with("allow-all");
        ApprovalCoordinator ap;
        int asked = 0;
        ap.set_emit([&asked](const std::string &t, const std::string &) {
            if (t == "permission_request") ++asked;
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap, g_home.string());
        const auto r = exe.execute("c1", "bash", R"({"command":"echo allow"})");
        CHECK(st(r) == 0 && msg(r) == "allow\n", "allow-all → 危险工具照跑");
        CHECK(asked == 0, "allow-all → 一句都不问");
    }
    {
        // deny：危险工具一律拒，只读工具不受影响
        Config cfg = config_with("deny");
        ApprovalCoordinator ap;
        int asked = 0;
        ap.set_emit([&asked](const std::string &t, const std::string &) {
            if (t == "permission_request") ++asked;
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap, g_home.string());
        const auto r = exe.execute("c2", "bash", R"({"command":"echo nope"})");
        CHECK(st(r) != 0 && msg(r).find("permission policy") != std::string::npos,
              "deny → 拒绝，理由说明是策略拒的");
        CHECK(asked == 0, "deny → 不问人（问了也白问）");

        const fs::path f = g_home / "r.txt";
        const auto ro = exe.execute("c3", "read", R"({"file_path":)" + q(f) + "}");
        CHECK(st(ro) == 0, "deny 只管危险工具，read 照读");
    }
    {
        // ask：真等裁决。裁决在 emit 回调里同步回传——await 是先登记后发帧，
        // 收帧的人立刻应答也接得住（不必靠另起线程与 sleep 来碰运气）
        Config cfg = config_with("ask");
        ApprovalCoordinator ap;
        std::string seen_tool;
        ap.set_emit([&ap, &seen_tool](const std::string &t, const std::string &payload) {
            if (t != "permission_request") return;
            const json ev = json::parse(payload);
            seen_tool = ev["tool"];
            ap.respond(ev["id"], true);
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap, g_home.string());
        const auto r = exe.execute("c4", "bash", R"({"command":"echo asked"})");
        CHECK(seen_tool == "bash", "ask → 发出 permission_request，点名是哪个工具");
        CHECK(st(r) == 0 && msg(r) == "asked\n", "用户放行 → 跑");
    }
    {
        Config cfg = config_with("ask");
        ApprovalCoordinator ap;
        ap.set_emit([&ap](const std::string &t, const std::string &payload) {
            if (t != "permission_request") return;
            const json ev = json::parse(payload);
            ap.respond(ev["id"], false);
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap, g_home.string());
        const auto r = exe.execute("c5", "bash", R"({"command":"echo denied"})");
        CHECK(st(r) != 0 && msg(r).find("denied by user") != std::string::npos,
              "用户拒绝 → 不跑，理由是人拒的（与策略拒的分开）");
    }
    {
        // 认不出的值按 ask：配置写错该多问一句，不该多放一次行
        Config cfg = config_with("yolo");
        ApprovalCoordinator ap;
        int asked = 0;
        ap.set_emit([&ap, &asked](const std::string &t, const std::string &payload) {
            if (t != "permission_request") return;
            ++asked;
            const json ev = json::parse(payload);
            ap.respond(ev["id"], false);
        });
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap, g_home.string());
        const auto r = exe.execute("c6", "bash", R"({"command":"echo yolo"})");
        CHECK(asked == 1, "permission 值认不出 → 走 ask，不是放行");
        CHECK(st(r) != 0, "拒绝生效");
    }

    printf("== 中止（ADR-0002 R8） ==\n");
    {
        Config cfg = config_with("allow-all");
        ApprovalCoordinator ap;
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        Executor exe(ctx, ap, g_home.string());

        // 手上没有在跑的：标记留给紧随其后的那次 execute 撞上
        exe.interrupt();
        const auto after = exe.execute("i1", "bash", R"({"command":"echo should-not-run"})");
        CHECK(intr(after) && st(after) != 0 && msg(after).find("should-not-run") == std::string::npos,
              "中止后紧接着的一次 execute 直接拒掉，命令没跑");

        exe.reset();
        const auto back = exe.execute("i2", "bash", R"({"command":"echo back"})");
        CHECK(st(back) == 0 && !intr(back), "reset 抹掉中止痕迹，下一轮照常");

        // 跑着的 bash 打得断：sleep 30 必须在 1 秒内被打掉
        const auto t0 = std::chrono::steady_clock::now();
        json r;
        std::thread worker([&] { r = exe.execute("i3", "bash", R"({"command":"sleep 30"})"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        exe.interrupt();
        worker.join();
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK(elapsed < std::chrono::seconds(5), "在跑的 bash 被打断，不是等它自己跑完");
        CHECK(intr(r), "结果标出这次是被中止的（与工具自己失败不是一回事）");
    }

    printf("\n== MCP 接进来了（ADR-0023）：一个分支，判的是代码在哪儿跑 ==\n");
    {
        // 全局 mcp.json 指向 stub server（HOME 已经指到临时目录）
        {
            std::ofstream f(g_home / ".realagent" / "mcp.json");
            f << R"({"mcpServers":{"stub":{"command":")" << MCP_STUB_SERVER << R"("}}})";
        }
        McpHub hub;
        auto lease = hub.open(g_home.string(), nlohmann::json::array());
        CHECK(lease.errors.empty() && lease.conns.size() == 1, "连上了 stub");

        Config cfg = config_with("allow-all");
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        ApprovalCoordinator ap;
        Executor exe(ctx, ap, g_home.string(), nullptr, 0, &lease);

        CHECK(find_tool("stub__echo") == nullptr, "内置那张表里没有它");
        CHECK(exe.find("stub__echo") != nullptr, "但 executor 查得到——内置在前，MCP 在后");
        CHECK(exe.find("read") != nullptr, "内置的照旧查得到");
        CHECK(exe.find("stub__nope") == nullptr, "没有的还是没有");
        const json *d = exe.find("stub__echo");
        CHECK(d && tool_dangerous(*d), "MCP 来的一律 dangerous（annotations 不可信）");

        const json r = exe.execute("c1", "stub__echo", R"({"message":"hi"})");
        CHECK(st(r) == 0, "调通了");
        CHECK(msg(r) == "Echo: hi", "转发用的是原名（stub 那头叫 echo，不带前缀）");

        const json bad = exe.execute("c2", "stub__nope", "{}");
        CHECK(st(bad) != 0 && msg(bad).find("unknown tool") != std::string::npos,
              "查不到的名字照旧是 unknown tool");
    }

    printf("\n== MCP 工具照走权限那条路 ==\n");
    {
        McpHub hub;
        auto lease = hub.open(g_home.string(), nlohmann::json::array());
        Config cfg = config_with("deny");
        CoreContext ctx{.config = &cfg, .emit_fn = nullptr};
        ApprovalCoordinator ap;
        Executor exe(ctx, ap, g_home.string(), nullptr, 0, &lease);
        const json r = exe.execute("c3", "stub__echo", R"({"message":"hi"})");
        CHECK(st(r) != 0 && msg(r).find("denied") != std::string::npos,
              "deny 挡得住——权限检查在派发之前，不分内置还是外部");
    }

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(g_home);
    printf(failures == 0 ? "\n全部通过\n" : "\n%d 项失败\n", failures);
    return failures == 0 ? 0 : 1;
}
