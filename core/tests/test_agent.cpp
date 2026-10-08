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

    // 客户端开一段新对话（id 由客户端给），回它在图里的 id
    int conv = 0;
    const auto top = [&home, &conv](Agents &pool, const std::string &client) {
        std::string err;
        const auto a = pool.open(client, home.string(), "c" + std::to_string(++conv), err);
        return a ? a->id() : 0;
    };

    printf("== 图：派生与边结构 ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;

        CHECK(pool.open("", "", "c0", err) == nullptr && !err.empty(), "workdir 必填");

        const int a = top(pool, "");
        CHECK(a > 0, "开得出第一段对话");

        const int b = pool.spawn(a, home.string(), {a}, {}, err); // a → b
        CHECK(b > 0 && err.empty(), "a 派生 b，并建立入边");

        CHECK(!pool.send(a, 999, "x"), "投给不存在的 agent 返回 false");
        CHECK(pool.send(a, b, "干活"), "a → b 有边，投得进去");
    }

    printf("== 能力模型：没有边就够不着，授不出自己没有的边（ADR-0019 §4b） ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
        const int a = top(pool, "");
        const int b = pool.spawn(a, home.string(), {}, {}, err);  // 派出去不管：a 不认识 b
        const int c = pool.spawn(a, home.string(), {a}, {}, err); // a → c

        CHECK(!pool.send(a, b, "x") && pool.send(a, c, "x"), "没有边就跟不存在一样；有边投得进去");

        CHECK(pool.spawn(a, home.string(), {b}, {}, err) == 0 && !err.empty(),
              "a 不认识 b，就不能让 b 认识新 agent");
        CHECK(pool.spawn(a, home.string(), {}, {b}, err) == 0 && !err.empty(),
              "a 不认识 b，就不能把 b 授给新 agent");

        const int d = pool.spawn(a, home.string(), {a, c}, {a, c}, err);
        CHECK(d > 0 && err.empty(), "授自己、授自己认识的：teamwork 形态建得出");
    }

    printf("== 对话：客户端只用 session_id 指它（ADR-0029） ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
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

        const int child = pool.spawn(again->id(), home.string(), {}, {}, err);
        CHECK(pool.node(child)->root() == "talk", "派生出来的属于同一段对话");
        CHECK(pool.node(child)->session_dir() == sessions_dir(home.string()) + "/sub",
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

        const int last = pool.spawn(a->id(), home.string(), {}, {}, err);
        pool.close_group("win-1");
        CHECK(!pool.find("win-1", a->session_id()) && !pool.node(last), "关组连派生的一起关");
        CHECK(pool.find("win-2", b->session_id()) != nullptr, "隔壁组的不受牵连");
        CHECK(top(pool, "win-1") > last, "id 不复用：模型历史里的旧 id 不会指到新 agent 上");
    }

    printf("== 发信人：收方认识发方才署名（ADR-0019 §5） ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
        const int a = top(pool, "");
        const int b = pool.spawn(a, home.string(), {a}, {a}, err); // a ⇄ b
        const int c = pool.spawn(a, home.string(), {a}, {}, err);  // a → c
        const auto pb = pool.node(b), pc = pool.node(c);

        pool.send(a, b, "TAGGED");
        pool.send(a, c, "PLAIN");
        CHECK(wait_messages(*pb, 1) && history(*pb)[0]["content"][0]["text"] == "[from " + std::to_string(a) + "] TAGGED",
              "b 有到 a 的边：带上是谁发的");
        CHECK(wait_messages(*pc, 1) && history(*pc)[0]["content"][0]["text"] == "PLAIN",
              "c 没有到 a 的边：跟人发的一模一样");
    }

    printf("== 完成通知沿入边逆向回流 ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
        const int a = top(pool, "");
        const int b = pool.spawn(a, home.string(), {a}, {}, err); // a → b
        const auto pa = pool.node(a);
        const auto snapshot = [pa] { return history(*pa); };
        const size_t before = snapshot().size();

        pool.send(a, b, "去干活"); // b 跑完会沿入边通知 a
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

    printf("== 只由完成通知唤醒的那一趟不再通知：环上不回声 ==\n");
    {
        Agents pool(ctx_of, approval);
        std::string err;
        const int a = top(pool, "");
        const int b = pool.spawn(a, home.string(), {a}, {a}, err); // a ⇄ b
        const auto pa = pool.node(a), pb = pool.node(b);

        pool.send(a, b, "去干活"); // b 收工 → 通知 a；a 那一趟只有通知 → 不再通知 b
        CHECK(wait_messages(*pa, 1), "a 收到了 b 的完成通知");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(history(*pb).size() == 1, "b 只有那一条活，没有被 a 的回声再叫醒");
    }

    printf("== 帧带上是哪段对话的（ADR-0029） ==\n");
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
        const int b = pool.spawn(a->id(), home.string(), {}, {}, err, {}, "干活");
        CHECK(wait_messages(*pool.node(b), 1), "子 agent 收到了");
        std::lock_guard<std::mutex> lk(fm);
        bool all = !frames.empty(), child = false;
        for (const std::string &f : frames)
        {
            const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
            all = all && j.is_object() && j.value("root", "") == a->session_id() && j.contains("session_id");
            child = child || j.value("session_id", "") == pool.node(b)->session_id();
        }
        CHECK(all, "每帧都是合法 JSON，root 都是那段对话");
        CHECK(child, "子 agent 的帧也在，session_id 是它自己的");
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
        const int me = top(pool, "");
        Executor exe(ctx, approval, home.string(), &pool, me, nullptr, nullptr, &defs);
        const auto spawn = [&](const std::string &agent) {
            nlohmann::json p{{"workdir", home.string()}, {"prompt", "干活"}};
            if (!agent.empty()) p["agent"] = agent;
            return exe.execute("call-1", "spawn", p.dump());
        };

        const nlohmann::json bad = spawn("nope");
        CHECK(bad.value("isError", false), "认不出的名字：当场失败");
        CHECK(text_of(bad).find("nope") != std::string::npos, "错误里点名是哪个名字");

        const nlohmann::json ok = spawn("builder");
        CHECK(!ok.value("isError", true), "认得的名字：派生成功");
        const int id = std::atoi(text_of(ok).c_str());
        const auto nb = id > 0 ? pool.node(id) : nullptr;
        CHECK(nb != nullptr, "新 agent 在图上");
        CHECK(nb && text_of(ok).find("(session " + nb->session_id() + ")") != std::string::npos,
              "结果里带它的 session_id：客户端回放时凭它把子 agent 嵌回这张卡片");
        const std::string sp = nb ? nb->system_prompt() : std::string();
        CHECK(sp.find("BODY_MARK") != std::string::npos, "那份正文接进了它的 system prompt");
        CHECK(sp.find("autonomous loop") != std::string::npos &&
                  sp.find("autonomous loop") < sp.find("BODY_MARK"),
              "core 那段与循环契约在它前面——派生出来的走的是同一个 system_prompt()");

        // 不给 agent 参数：与加这个功能之前一个字不差
        const nlohmann::json plain = spawn("");
        const int id2 = std::atoi(text_of(plain).c_str());
        const auto nb2 = id2 > 0 ? pool.node(id2) : nullptr;
        CHECK(nb2 && nb2->system_prompt().find("BODY_MARK") == std::string::npos,
              "不给 agent 参数就一个字都不接");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
