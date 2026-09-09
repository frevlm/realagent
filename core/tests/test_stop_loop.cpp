/*
 * test_stop_loop.cpp — 出口那条路，端到端（ADR-0025）
 *
 * 对手是本文件里起的一个假端点：说 openai-chat，按请求里的模型名挑一份预置的 SSE 回。
 * 于是「主模型没调工具 → 问小模型 → 打回 / 收工」这条路整条走得通，不必真花钱。
 *
 * 验的全是这次改动里会坏的：
 *   - 主模型没调工具**不再等于收工**：裁判说没干完就打回，主模型接着跑
 *   - 打回那条消息进历史，带 supervisor 标记（下一次判定要认得出它不是用户说的）
 *   - 裁判说干完了才收工，recap 落在 agent_end 帧里
 *   - **裁判那次调用的正文一个字都不往客户端推**——那是给它自己看的
 *   - 裁判一直说没干完时，连着三轮没跑工具就强制收工（kMaxStall）
 *   - 没配小模型就没有裁判：一次都不问，行为退回这个功能之前
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.hpp"
#include "agent/approval.hpp"
#include "agent/context.hpp"
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

static bool has(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

/* —— 假端点：一个线程、一个监听套接字、按模型名挑回答 ——
 *
 * 只说 openai-chat 一套：这里验的是 loop 的出口，不是协议解析（那是 test_llm 的活）。 */
class FakeEndpoint {
  public:
    FakeEndpoint()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0; // 内核挑一个空闲端口，测试之间不撞
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || ::listen(fd_, 8) != 0)
        {
            printf("  FAIL: 假端点起不来\n");
            return;
        }
        socklen_t len = sizeof a;
        ::getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len);
        port_ = ntohs(a.sin_port);
        th_ = std::thread([this] { serve(); });
    }

    ~FakeEndpoint()
    {
        stop_.store(true);
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        if (th_.joinable()) th_.join();
    }

    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/v1"; }

    /* 主模型每被问一次，就按顺序发下一份回答（问多了就重复最后一份） */
    void main_says(std::vector<std::string> texts) { main_ = std::move(texts); }
    /* 小模型同上。这些是裁判要回的那段 JSON */
    void small_says(std::vector<std::string> texts) { small_ = std::move(texts); }

    int main_calls() const { return main_n_.load(); }
    int small_calls() const { return small_n_.load(); }

  private:
    static std::string sse(const std::string &text)
    {
        nlohmann::json d{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}};
        nlohmann::json fin{
            {"choices", nlohmann::json::array({{{"delta", nlohmann::json::object()}, {"finish_reason", "stop"}}})}};
        return "data: " + d.dump() + "\n\ndata: " + fin.dump() + "\n\ndata: [DONE]\n\n";
    }

    static const std::string &pick(const std::vector<std::string> &v, int n)
    {
        static const std::string empty = "(nothing)";
        if (v.empty()) return empty;
        return v[(size_t)n < v.size() ? (size_t)n : v.size() - 1];
    }

    void serve()
    {
        while (!stop_.load())
        {
            const int c = ::accept(fd_, nullptr, nullptr);
            if (c < 0) return;

            // 请求整份读进来：只要认出模型名就够，不必真解 HTTP
            std::string req;
            char buf[4096];
            for (;;)
            {
                const ssize_t n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, (size_t)n);
                // 头里的 Content-Length 到齐了就不等了（curl 不会主动关这一端）
                const size_t hdr = req.find("\r\n\r\n");
                if (hdr == std::string::npos) continue;
                const size_t cl = req.find("Content-Length: ");
                if (cl == std::string::npos) break;
                const size_t want = (size_t)std::stoul(req.substr(cl + 16));
                if (req.size() - (hdr + 4) >= want) break;
            }

            const bool is_small = has(req, "m-small");
            const std::string body =
                sse(is_small ? pick(small_, small_n_++) : pick(main_, main_n_++));
            const std::string resp =
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + body;
            ::send(c, resp.data(), resp.size(), 0);
            ::close(c);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::vector<std::string> main_, small_;
    std::atomic<int> main_n_{0}, small_n_{0};
};

/* 收到的事件帧。agent 线程往里写，主线程读——一把锁就够 */
struct Frames {
    mutable std::mutex mtx;
    std::vector<std::pair<std::string, std::string>> v;

    void add(const std::string &t, const std::string &p)
    {
        std::lock_guard<std::mutex> lk(mtx);
        v.push_back({t, p});
    }
    std::vector<std::pair<std::string, std::string>> snapshot() const
    {
        std::lock_guard<std::mutex> lk(mtx);
        return v;
    }
    int count(const std::string &t) const
    {
        int n = 0;
        for (const auto &e : snapshot())
            if (e.first == t) ++n;
        return n;
    }
    std::string last(const std::string &t) const
    {
        std::string out;
        for (const auto &e : snapshot())
            if (e.first == t) out = e.second;
        return out;
    }
    std::string joined(const std::string &t) const
    {
        std::string out;
        for (const auto &e : snapshot())
            if (e.first == t) out += e.second;
        return out;
    }
};

/* 等这一趟收工（agent_end）。等不到返回 false，不无限挂着 */
static bool wait_end(const Frames &f, int n = 1)
{
    for (int i = 0; i < 600; ++i) // 最多 6 秒
    {
        if (f.count("agent_end") >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static nlohmann::json history(const Agent &a)
{
    nlohmann::json out = nlohmann::json::array();
    Session::read(a.session_dir(), a.session_id(), out);
    return out;
}

int main()
{
    const fs::path home =
        fs::temp_directory_path() / ("realagent-stoploop-test-" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);

    FakeEndpoint ep;
    {
        nlohmann::json s{{"protocol", "openai-chat"},
                         {"base_url", ep.base_url()},
                         {"api_key", "k"},
                         {"model", "m-main"},
                         {"small_model", "m-small"},
                         {"permission", "allow-all"}};
        std::ofstream(home / ".realagent" / "settings.json") << s.dump();
    }

    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: 配置加载失败\n");
        return 1;
    }

    printf("== 裁判说没干完：打回，主模型接着跑 ==\n");
    {
        Frames f;
        CoreContext ctx{.config = &*cfg,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        ep.main_says({"MAIN_FIRST", "MAIN_SECOND"});
        ep.small_says({R"({"recap":"半截","done":false,"reason":"JUDGE_REASON 还没跑测试"})",
                       R"({"recap":"RECAP_DONE","done":true,"reason":""})"});

        Agent a(ctx, approval, home.string(), 1);
        a.post("干活");
        CHECK(wait_end(f), "收工了（等到 agent_end）");

        CHECK(ep.main_calls() == 2, "主模型跑了两轮——第一轮没调工具但没收工，是被打回的");
        CHECK(ep.small_calls() == 2, "裁判问了两次，一轮一次");

        const nlohmann::json h = history(a);
        std::string kicked;
        for (const auto &m : h)
            if (m.value("role", "") == "user")
                for (const auto &b : m["content"])
                    if (b.value("type", "") == "text" && has(b.value("text", ""), "JUDGE_REASON"))
                        kicked = b.value("text", "");
        CHECK(!kicked.empty(), "打回的理由进了历史——主模型看得见它为什么被打回");
        CHECK(kicked.rfind(kSupervisorTag, 0) == 0,
              "打回那条带 supervisor 标记，下一次判定认得出它不是用户说的");

        const std::string ended = f.last("agent_end");
        CHECK(has(ended, "RECAP_DONE"), "收工那一帧带 recap");

        const std::string deltas = f.joined("message_update");
        CHECK(has(deltas, "MAIN_FIRST") && has(deltas, "MAIN_SECOND"), "主模型的正文照推");
        CHECK(!has(deltas, "RECAP_DONE") && !has(deltas, "JUDGE_REASON"),
              "裁判那次调用的正文一个字都没推给客户端");
    }

    printf("== 裁判一直说没干完：连着三轮没跑工具就强制收工 ==\n");
    {
        Frames f;
        CoreContext ctx{.config = &*cfg,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int before = ep.main_calls();
        ep.main_says({"STILL_TALKING"});
        ep.small_says({R"({"recap":"没完","done":false,"reason":"接着干"})"});

        Agent a(ctx, approval, home.string(), 3);
        a.post("干活");
        CHECK(wait_end(f), "还是收工了——不设轮数上限，但推不动了要认");
        CHECK(ep.main_calls() - before == 3,
              "主模型跑了三轮就打住：打回两次，第三次判定强制收工（kMaxStall）");
    }

    printf("== 没配小模型：没有裁判，没调工具就算干完了 ==\n");
    {
        // settings.json 换一份，去掉 small_model。**不回落主模型**（ADR-0010）——
        // 行为退回这个功能之前
        nlohmann::json s{{"protocol", "openai-chat"},
                         {"base_url", ep.base_url()},
                         {"api_key", "k"},
                         {"model", "m-main"},
                         {"permission", "allow-all"}};
        std::ofstream(home / ".realagent" / "settings.json") << s.dump();
        auto c2 = Config::load();
        if (!c2)
        {
            printf("  FAIL: 配置重载失败\n");
            return 1;
        }

        Frames f;
        CoreContext ctx{.config = &*c2,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int before = ep.small_calls();
        ep.main_says({"ONLY_ANSWER"});

        Agent a(ctx, approval, home.string(), 2);
        a.post("说句话");
        CHECK(wait_end(f), "收工了");
        CHECK(ep.small_calls() == before, "一次都没问裁判——没配就是没有裁判");
        CHECK(has(f.last("agent_end"), "\"recap\":\"\""), "没有裁判就没有 recap，字段仍在（帧形状恒定）");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
