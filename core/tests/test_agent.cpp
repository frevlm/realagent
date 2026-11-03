/*
 * test_agent.cpp — agent 的收件箱与线程生命周期（ADR-0019）
 *
 * 这里不测对话质量，测的是 Agent 内化那条线程之后最容易坏的三件事：
 *   - 空收件箱时析构不挂（loop 阻塞在条件变量上，closing_ 得叫得醒它）
 *   - 投进去的消息排队、不丢、按顺序
 *   - 正在跑的时候析构也不挂（先 interrupt 再 join，不等它自己跑完）
 *
 * 端点没配齐（默认树里 model 为空），LLM 调用当场失败——正好：
 * 这里要验的是消息进出收件箱，不是模型回了什么。
 *
 * 之后验对话表与 spawn（ADR-0033）：spawn 阻塞到子 agent 跑完、中断传得下去、
 * `agent` 参数（ADR-0024 §8）认得的名字把那份正文接进子 agent 的 system prompt。
 */
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/approval.hpp"
#include "agent/catalog.hpp"
#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "agent/session.hpp"
#include "config.hpp"

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

/* 工具结果里那几段文字接起来 */
static std::string text_of(const nlohmann::json &r)
{
    std::string s;
    for (const auto &b : r.value("content", nlohmann::json::array()))
        if (b.value("type", std::string()) == "text") s += b.value("text", std::string());
    return s;
}

/* 问这个 agent 记了什么：**问盘，不问内存**。idle 的 agent 内存里那份已经还回去了
 * （ADR-0019 §7），盘上那份才是一直在的那一份。 */
static nlohmann::json history(const Agent &a)
{
    nlohmann::json out = nlohmann::json::array();
    Session::read(a.session_dir(), a.session_id(), out);
    return out;
}

/* 等 agent 的历史里攒够 n 条消息。等不到就返回 false，不无限挂着。 */
static bool wait_messages(const Agent &a, size_t n)
{
    for (int i = 0; i < 400; ++i) // 最多 4 秒
    {
        if (history(a).size() >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main()
{
    const fs::path home =
        fs::temp_directory_path() / ("realagent-agent-test-" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);
    // spawn 是危险工具。这里验的是它派生出什么，不是权限链——权限有 test_tools 管
    std::ofstream(home / ".realagent" / "settings.json") << R"({"permission":"allow-all"})";

    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: 配置加载失败\n");
        return 1;
    }
    CoreContext ctx{.config = &*cfg, .pricing = nullptr, .emit_fn = nullptr};
    ApprovalCoordinator approval;
    const ContextOf ctx_of = [&ctx](const std::string &) { return ctx; };

    printf("== 空收件箱时析构不挂 ==\n");
    {
        // loop 此刻正阻塞在条件变量上。closing_ 叫不醒它的话，这里 join 就是永远
        Agent a(ctx, approval, home.string(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(true, "构造后立刻析构，join 得回来");

    printf("== 收件箱排队、不丢、按顺序 ==\n");
    {
        Agent a(ctx, approval, home.string(), 2);
        a.post("第一条");
        a.post("第二条"); // 第一条还在跑的时候投进来，要排队不要覆盖

        CHECK(wait_messages(a, 2), "两条都被处理，没有一条被吞掉");
        const nlohmann::json m = history(a);
        CHECK(m.size() >= 2 && m[0]["role"] == "user" && m[1]["role"] == "user",
              "两条都以 user 身份入账——凡是从 agent 外面来的输入都是 user");
        CHECK(m.size() >= 2 && m[0]["content"][0]["text"] == "第一条" &&
                  m[1]["content"][0]["text"] == "第二条",
              "顺序就是投递顺序，收件箱是队列不是集合");
    }
    CHECK(true, "处理完之后析构，join 得回来");

    printf("== idle 不把对话历史留在内存里（ADR-0019 §7）==\n");
    {
        Agent a(ctx, approval, home.string(), 4);
        a.post("记一条");
        CHECK(wait_messages(a, 1), "盘上记下了");
        bool freed = false;
        for (int i = 0; i < 400 && !freed; ++i) // 收件箱空了 loop 才丢，等它一下
        {
            freed = a.resident() == 0;
            if (!freed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(freed, "跑完且收件箱空了，内存里那份还回去了");
        CHECK(history(a).size() >= 1, "盘上那份一直在——丢的是副本，不是信息");

        // 再投一条：醒来先把历史读回来，接着往下写，不是从空的重来
        a.post("再记一条");
        CHECK(wait_messages(a, 2), "醒来读回了旧历史，新的接在后面");
    }

    printf("== 正在跑的时候析构不挂 ==\n");
    {
        Agent a(ctx, approval, home.string(), 3);
        a.post("在跑");
        // 不等它跑完就走人：析构要先 interrupt 再 join，不能傻等
    }
    CHECK(true, "带着未处理的消息析构，也 join 得回来");

    printf("== 对话：客户端只用 session_id 指它（ADR-0029） ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
        CHECK(pool.open("", "", "c0", err) == nullptr && !err.empty(), "workdir 必填");
        const auto a = pool.open("w", home.string(), "talk", err);
        CHECK(a && pool.open("w", home.string(), "talk", err) == a, "同一段对话再来：还是那一个 agent");
        CHECK(pool.open("w", home.string(), "../x", err) == nullptr, "id 不许带路径");

        a->post("记一条");
        CHECK(wait_messages(*a, 1), "落盘了");
        pool.close_group("w");
        const auto again = pool.open("w2", home.string(), "talk", err);
        CHECK(again && wait_messages(*again, 1), "关掉后再来：从盘上读回来，接着那段往下说");
        CHECK(pool.open("w3", home.string(), "talk", err) == nullptr && !err.empty(),
              "在别的窗口里开着：不许再开一个往同一个文件里写");
        CHECK(pool.state("w3", "talk") == "elsewhere", "清单里看得出它在别的窗口里开着");

        const auto child = pool.spawn(again->id(), home.string(), {}, {}, err);
        CHECK(child && child->root() == "talk", "派生出来的属于同一段对话");
        CHECK(child && child->session_dir() == sessions_dir(home.string()) + "/sub",
              "子 agent 落在对话那一头的 sub/，不进对话清单");
    }

    printf("== 组：跨组一律当不存在（ADR-0021） ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
        const auto a = pool.open("win-1", home.string(), "g1", err);
        const auto b = pool.open("win-2", home.string(), "g2", err);

        CHECK(pool.find("win-1", a->session_id()) && !pool.find("win-2", a->session_id()),
              "别的组找不到——不区分「不存在」与「不是你的」");
        pool.close_group("win-1");
        CHECK(!pool.find("win-1", a->session_id()), "关组关掉它的对话");
        CHECK(pool.find("win-2", b->session_id()) != nullptr, "隔壁组的不受牵连");
        CHECK(pool.open("win-1", home.string(), "g3", err)->id() > b->id(),
              "id 不复用：审批按 id 认人，旧 id 不会指到新 agent 上");
    }

    printf("== spawn：阻塞到子 agent 跑完，结果带它的会话（ADR-0033） ==\n");
    {
        std::vector<std::string> frames;
        std::mutex fm;
        const ContextOf record = [&](const std::string &) {
            CoreContext c = ctx;
            c.emit_fn = [&](const std::string &, const std::string &payload) {
                std::lock_guard<std::mutex> lk(fm);
                frames.push_back(payload);
            };
            return c;
        };
        Agents pool(record, approval);
        std::string err;
        const auto a = pool.open("", home.string(), "stamped", err);
        Executor exe(ctx, approval, home.string(), &pool, a->id());

        // 端点没配齐：子 agent 第一个 turn 就失败，这个错误要原样交回派生方
        const nlohmann::json r = exe.execute("call-1", "spawn", R"({"prompt":"干活"})");
        const std::string out = text_of(r);
        CHECK(r.value("isError", false) && out.find("stopped on an error") != std::string::npos &&
                  out.find(endpoint_config_error(*cfg)) != std::string::npos,
              "子 agent 停在错误上：spawn 报错，带着那个错误的原文");
        const size_t at = out.rfind("(session ");
        const std::string sid = at == std::string::npos ? "" : out.substr(at + 9, out.size() - at - 10);
        CHECK(!sid.empty() && out.back() == ')', "结果末尾带子 agent 的会话 id：客户端回放时凭它嵌回卡片");

        nlohmann::json sub = nlohmann::json::array();
        Session::read(sessions_dir(home.string()) + "/sub", sid, sub);
        CHECK(sub.size() >= 1 && sub[0]["content"][0]["text"] == "干活",
              "返回时子 agent 已经跑完：prompt 作为第一条 user 消息落在 sub/ 里");

        std::lock_guard<std::mutex> lk(fm);
        bool all = !frames.empty(), child = false;
        for (const std::string &f : frames)
        {
            const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            all = all && j.is_object() && j.value("root", "") == a->session_id() && j.contains("session_id");
            child = child || j.value("session_id", "") == sid;
        }
        CHECK(all, "每帧都是合法 JSON，root 都是那段对话");
        CHECK(child, "子 agent 的帧也在，session_id 是它自己的");
    }

    printf("== 子 agent 不能再派生 ==\n");
    {
        Executor exe(ctx, approval, home.string()); // 子 agent 的 executor 不拿 pool
        const nlohmann::json r = exe.execute("call-1", "spawn", R"({"prompt":"x"})");
        CHECK(r.value("isError", false) && text_of(r).find("not available") != std::string::npos,
              "没有 pool 的 executor 拒绝 spawn");
    }

    printf("== 中断传到子 agent：spawn 不等它自己跑完 ==\n");
    {
        // 子 agent 的 workdir 里放一个 30 秒的 UserPromptSubmit hook：不中断它就卡 30 秒
        const fs::path slow = home / "slow";
        fs::create_directories(slow / ".realagent" / "hooks");
        std::ofstream(slow / ".realagent" / "hooks" / "hooks.json")
            << R"({"hooks":{"UserPromptSubmit":[{"hooks":[{"type":"command","command":"sleep 30"}]}]}})";

        Agents pool(ctx_of, approval);
        std::string err;
        const auto a = pool.open("", home.string(), "intr", err);
        Executor exe(ctx, approval, home.string(), &pool, a->id());
        const nlohmann::json p{{"prompt", "慢活"}, {"workdir", slow.string()}};
        const auto t0 = std::chrono::steady_clock::now();
        nlohmann::json r;
        std::thread worker([&] { r = exe.execute("call-1", "spawn", p.dump()); });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        exe.interrupt();
        worker.join();
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5), "30 秒的活被打断，不是等它跑完");
        CHECK(r.value("interrupted", false), "结果标出是被中断的");
    }

    printf("== 最后一个收工帧发出时，问得到这段对话停了 ==\n");
    {
        // 客户端（MCP 桥）靠「收到 agent_end 就问 GET /sessions」判定一段对话停没停。
        // 在帧出口里同步地问，就是客户端最快能问到的那一刻
        Agents *pool_ptr = nullptr;
        std::vector<std::string> states;
        std::mutex fm;
        const ContextOf probe = [&](const std::string &client) {
            CoreContext c = ctx;
            c.emit_fn = [&, client](const std::string &type, const std::string &payload) {
                if (type != "agent_end") return;
                const nlohmann::json j = nlohmann::json::parse(payload);
                if (j.value("session_id", "") != j.value("root", "")) return; // 只看对话本身
                std::lock_guard<std::mutex> lk(fm);
                states.push_back(pool_ptr->state(client, j.value("root", "")));
            };
            return c;
        };
        Agents pool(probe, approval);
        pool_ptr = &pool;
        std::string err;
        pool.open("p", home.string(), "settle", err)->post("干活");
        for (int i = 0; i < 400; ++i)
        {
            {
                std::lock_guard<std::mutex> lk(fm);
                if (!states.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> lk(fm);
        CHECK(states.size() == 1 && states[0] == "", "收工帧发出时，对话已经不算在跑");
    }

    printf("== spawn 的 agent 参数：认得的接上正文，认不得当场失败（ADR-0024 §8） ==\n");
    {
        /* 隐式 plugin（`~/.realagent/agents/`）里放一份定义：名字不带前缀，取文件名。
         * 走真的扫盘，于是「模型在 system prompt 里看见的名字」与「spawn 认的名字」
         * 是同一个来源——这正是这个参数唯一会坏的地方。 */
        fs::create_directories(home / ".realagent" / "agents");
        std::ofstream(home / ".realagent" / "agents" / "builder.md")
            << "---\ndescription: 只改一两个文件\n---\nBODY_MARK 一个文件最好。\n";
        const std::vector<AgentDef> defs = scan_agent_defs(home.string());
        CHECK(defs.size() == 1 && defs[0].name == "builder", "扫到一份，名字取文件名");

        Agents pool(ctx_of, approval);
        std::string err;
        const auto me = pool.open("", home.string(), "defs", err);
        Executor exe(ctx, approval, home.string(), &pool, me->id(), nullptr, nullptr, &defs);
        const nlohmann::json bad = exe.execute("call-1", "spawn", R"({"prompt":"干活","agent":"nope"})");
        CHECK(bad.value("isError", false), "认不出的名字：当场失败");
        CHECK(text_of(bad).find("nope") != std::string::npos, "错误里点名是哪个名字");
        CHECK(text_of(exe.execute("call-2", "spawn", R"({"prompt":"干活","agent":"builder"})")).find("(session ") !=
                  std::string::npos,
              "认得的名字：子 agent 派生出来跑过了");

        // 子 agent 跑完就析构了，它的 system prompt 从 Agents::spawn 那一处看
        const auto nb = pool.spawn(me->id(), home.string(), defs[0].body, {}, err);
        const std::string sp = nb ? nb->system_prompt() : std::string();
        CHECK(sp.find("BODY_MARK") != std::string::npos, "那份正文接进了它的 system prompt");
        CHECK(sp.find("autonomous loop") != std::string::npos &&
                  sp.find("autonomous loop") < sp.find("BODY_MARK"),
              "core 那段与循环契约在它前面——派生出来的走的是同一个 system_prompt()");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
