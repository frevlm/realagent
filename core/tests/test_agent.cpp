/*
 * test_agent.cpp — agent 的收件箱与线程生命周期（ADR-0019）
 *
 * 这里不测对话质量，测的是 Agent 内化那条线程之后最容易坏的三件事：
 *   - 空收件箱时析构不挂（loop 阻塞在条件变量上，closing_ 得叫得醒它）
 *   - 投进去的消息排队、不丢、按顺序
 *   - 正在跑的时候析构也不挂（先 interrupt 再 join，不等它自己跑完）
 *
 * 端点没配（默认树里 base_url 为空），LLM 调用当场失败——正好：
 * 这里要验的是消息进出收件箱，不是模型回了什么。
 *
 * 图那几段之后还验一件事：`spawn` 的 `agent` 参数（ADR-0024 §8）——认得的名字把那份
 * 正文接进新 agent 的 system prompt，认不得的当场失败且**不留下一个新 agent**。
 */
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "agent/agent.hpp"
#include "agent/agent_defs.hpp"
#include "agent/agents.hpp"
#include "agent/approval.hpp"
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

    printf("== 图：创建与边结构 ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;

        CHECK(pool.create("", 0, {}, {}, err) == 0 && !err.empty(), "workdir 必填");

        const int a = pool.create(home.string(), 0, {}, {}, err);
        CHECK(a > 0, "建得出第一个 agent");

        // a 派生 b，入边填 a
        const int b = pool.create(home.string(), a, {a}, {}, err);
        CHECK(b > 0 && err.empty(), "a 派生 b，并建立入边");

        const int c = pool.create(home.string(), a, {}, {}, err);
        CHECK(c > 0, "a 再派生一个 c");

        CHECK(!pool.post(999, "x"), "投给不存在的 agent 返回 false");
        CHECK(pool.post(b, "干活"), "投得进去");
        CHECK(pool.list().size() == 3, "清单列出全部节点");
    }

    printf("== 完成通知沿入边逆向回流 ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;
        const int a = pool.create(home.string(), 0, {}, {}, err);
        const int b = pool.create(home.string(), a, {a}, {}, err); // a → b
        Agent *pa = pool.find(a);
        const auto snapshot = [pa] { return history(*pa); };
        const size_t before = snapshot().size();

        pool.post(b, "去干活"); // b 跑完会沿入边通知 a
        nlohmann::json m;
        for (int i = 0; i < 400; ++i)
        {
            m = snapshot();
            if (m.size() > before) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(m.size() > before, "b 跑完，a 的收件箱里多了一条");
        const std::string got = m.empty() ? "" : m.back()["content"][0]["text"].get<std::string>();
        CHECK(got.find(std::to_string(b)) != std::string::npos && got.find("done") != std::string::npos,
              "通知点名是谁跑完了");
        CHECK(!m.empty() && m.back()["role"] == "user",
              "role 是 user——凡是从 agent 外面来的输入都是 user");
    }

    printf("== close：关闭 agent ==\n");
    {
        Agents pool(ctx, approval);
        std::string err;
        const int a = pool.create(home.string(), 0, {}, {}, err);
        const int b = pool.create(home.string(), a, {a}, {}, err);
        pool.close(b);
        CHECK(pool.find(b) == nullptr && pool.list().size() == 1, "b 没了，清单里也没有它了");
    }

    printf("== 会话落点由「谁创建的」决定，不是一个参数（ADR-0021） ==\n");
    {
        const fs::path wd = home / "wd";
        fs::create_directories(wd);
        Agents pool(ctx, approval);
        std::string err;
        const int top = pool.create(wd.string(), 0, {}, {}, err);
        const int sub = pool.create(wd.string(), top, {top}, {}, err);

        const std::string dtop = pool.find(top)->session_dir();
        const std::string dsub = pool.find(sub)->session_dir();
        CHECK(dtop == (wd / ".realagent" / "sessions").string(), "客户端建的落 sessions/");
        CHECK(dsub == (wd / ".realagent" / "sessions" / "sub").string(), "派生的落 sessions/sub/");
        CHECK(dtop != dsub, "两个落点不同——清单只扫顶层，于是 sub 的不进列表");
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

        Agents pool(ctx, approval);
        std::string err;
        const int me = pool.create(home.string(), 0, {}, {}, err);
        Executor exe(ctx, approval, home.string(), &pool, me, nullptr, nullptr, &defs);
        const auto spawn = [&](const std::string &agent) {
            nlohmann::json p{{"workdir", home.string()}, {"prompt", "干活"}};
            if (!agent.empty()) p["agent"] = agent;
            return exe.execute("call-1", "spawn", p.dump());
        };

        const nlohmann::json bad = spawn("nope");
        CHECK(bad.value("isError", false), "认不出的名字：当场失败");
        CHECK(text_of(bad).find("nope") != std::string::npos, "错误里点名是哪个名字");
        CHECK(pool.list().size() == 1, "失败就没有新 agent——不悄悄派生一个没接上正文的");

        const nlohmann::json ok = spawn("builder");
        CHECK(!ok.value("isError", true), "认得的名字：派生成功");
        const int id = std::atoi(text_of(ok).c_str());
        const Agent *nb = id > 0 ? pool.find(id) : nullptr;
        CHECK(nb != nullptr, "新 agent 在图上");
        const std::string sp = nb ? nb->system_prompt() : std::string();
        CHECK(sp.find("BODY_MARK") != std::string::npos, "那份正文接进了它的 system prompt");
        CHECK(sp.find("`stop`") != std::string::npos &&
                  sp.find("`stop`") < sp.find("BODY_MARK"),
              "core 那段与 stop 契约在它前面——派生出来的走的是同一个 system_prompt()");

        // 不给 agent 参数：与加这个功能之前一个字不差
        const nlohmann::json plain = spawn("");
        const int id2 = std::atoi(text_of(plain).c_str());
        const Agent *nb2 = id2 > 0 ? pool.find(id2) : nullptr;
        CHECK(nb2 && nb2->system_prompt().find("BODY_MARK") == std::string::npos,
              "不给 agent 参数就一个字都不接");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
