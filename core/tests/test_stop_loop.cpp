/*
 * test_stop_loop.cpp — 出口那条路，端到端（ADR-0025、ADR-0032）
 *
 * 对手是本文件里起的一个假端点：一个 HTTP/1.1 桩，同时扮 revlm 的 /v1/models、/v1/systemone
 * 与聊天（按请求里的模型名挑一份预置的 SSE 回）。send_revlm 先说 h2，桩回 HTTP/1.1，
 * 于是退回 curl（transport.hpp），整条路走得通，不必真花钱。
 *
 * 验的全是这次改动里会坏的：
 *   - 主模型没调工具**不等于收工**：decision_model 说没干完就打回，主模型接着跑，**小模型一次不问**
 *   - 打回那条消息进历史，带 supervisor 标记（下一次判定要认得出它不是用户说的）
 *   - decision_model 说干完了才问小模型写 recap，recap 落在 agent_end 帧里
 *   - **recap 那次调用的正文一个字都不往客户端推**——那是给 recap 自己用的
 *   - 一直说没干完时，连着三轮没跑工具就强制收工（kMaxStall）
 *   - 没配 decision_model、或端点不是 revlm：不问 System One，行为退回这个功能之前
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
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.hpp"
#include "agent/approval.hpp"
#include "agent/context.hpp"
#include "agent/session.hpp"
#include "agent/verdict.hpp"
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

/* —— 假端点：一个线程、一个监听套接字、按路径与模型名挑回答 ——
 *
 * 聊天只说 openai-chat 一套：这里验的是 loop 的出口，不是协议解析（那是 test_llm 的活）。 */
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

    /* revlm 的 base_url 不带 /v1（路由自己补）；openai-chat 的带 */
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    /* 主模型每被问一次，就按顺序发下一份回答（问多了就重复最后一份） */
    void main_says(std::vector<std::string> texts) { main_ = std::move(texts); }
    /* 小模型同上。这些是 recap 的正文 */
    void small_says(std::vector<std::string> texts) { small_ = std::move(texts); }
    /* System One 每被问一次，就按顺序回下一个 done 的把握（问多了就重复最后一个） */
    void decision_says(std::vector<double> noul) { decision_ = std::move(noul); }

    int main_calls() const { return main_n_.load(); }
    int small_calls() const { return small_n_.load(); }
    int decision_calls() const { return decision_n_.load(); }
    std::string last_decision_request() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_decision_;
    }

  private:
    static std::string sse(const std::string &text)
    {
        nlohmann::json d{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}};
        nlohmann::json fin{
            {"choices", nlohmann::json::array({{{"delta", nlohmann::json::object()}, {"finish_reason", "stop"}}})}};
        return "data: " + d.dump() + "\n\ndata: " + fin.dump() + "\n\ndata: [DONE]\n\n";
    }

    template <class T>
    static const T &pick(const std::vector<T> &v, int n)
    {
        static const T empty{};
        if (v.empty()) return empty;
        return v[(size_t)n < v.size() ? (size_t)n : v.size() - 1];
    }

    static std::string json_reply(const nlohmann::json &j)
    {
        const std::string body = j.dump();
        return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    }

    /* 按请求行路由。revlm::net 的开场白（REALURL/1、h2 的 PRI）不是 HTTP 请求，回一句 400，
     * send_revlm 据此退回 curl */
    std::string respond(const std::string &req)
    {
        if (!req.starts_with("GET ") && !req.starts_with("POST ")) return "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        const std::string line = req.substr(0, req.find("\r\n"));
        if (has(line, "/models"))
        {
            nlohmann::json data = nlohmann::json::array();
            for (const char *id : {"m-main", "m-small", "m-decide"}) data.push_back({{"id", id}, {"protocol", "chat_completions"}});
            return json_reply({{"data", data}});
        }
        if (has(line, "/systemone"))
        {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                last_decision_ = req;
            }
            return json_reply({{"model", "jev-1.14.0"},
                               {"answers", {{"done", {{"type", "noul"}, {"noul", pick(decision_, decision_n_++)}}}}},
                               {"usage", {{"input_tokens", 10}, {"output_tokens", 2}}}});
        }
        const bool is_small = has(req, "m-small");
        const std::string text = is_small ? pick(small_, small_n_++) : pick(main_, main_n_++);
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + sse(text);
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

            if (req.empty()) // 只连不说话的探测连接
            {
                ::close(c);
                continue;
            }
            const std::string resp = respond(req);
            ::send(c, resp.data(), resp.size(), 0);
            ::close(c);
        }
    }

    int fd_ = -1;
    int port_ = 0;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::vector<std::string> main_, small_;
    std::vector<double> decision_;
    std::atomic<int> main_n_{0}, small_n_{0}, decision_n_{0};
    mutable std::mutex mtx_;
    std::string last_decision_;
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

/* settings.json 换一份并重载 */
static std::optional<Config> reload(const fs::path &home, const nlohmann::json &s)
{
    std::ofstream(home / ".realagent" / "settings.json") << s.dump();
    auto c = Config::load();
    if (!c)
    {
        printf("  FAIL: 配置加载失败\n");
        return std::nullopt;
    }
    return std::move(*c);
}

int main()
{
    const fs::path home =
        fs::temp_directory_path() / ("realagent-stoploop-test-" + std::to_string(::getpid()));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);

    FakeEndpoint ep;
    const nlohmann::json revlm{{"protocol", "revlm"},
                               {"base_url", ep.base_url()},
                               {"api_key", "k"},
                               {"model", "m-main"},
                               {"small_model", "m-small"},
                               {"decision_model", "m-decide"},
                               {"permission", "allow-all"}};
    auto cfg = reload(home, revlm);
    if (!cfg) return 1;

    printf("== System One 说没干完：打回，主模型接着跑，小模型一次不问 ==\n");
    {
        Frames f;
        CoreContext ctx{.config = &*cfg,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        ep.main_says({"MAIN_FIRST", "MAIN_SECOND"});
        ep.decision_says({0.2, 0.9});
        ep.small_says({"RECAP_DONE"});

        Agent a(ctx, approval, home.string(), 1);
        a.post("干活");
        CHECK(wait_end(f), "收工了（等到 agent_end）");

        CHECK(ep.main_calls() == 2, "主模型跑了两轮——第一轮没调工具但没收工，是被打回的");
        CHECK(ep.decision_calls() == 2, "System One 问了两次，一轮一次");
        CHECK(ep.small_calls() == 1, "小模型只在收工那一轮写 recap，打回那轮没问");

        const std::string asked = ep.last_decision_request();
        const nlohmann::json body = nlohmann::json::parse(asked.substr(asked.find("\r\n\r\n") + 4), nullptr, false);
        CHECK(has(asked, "POST /v1/systemone") && has(asked, "Authorization: Bearer k"), "POST /v1/systemone，带 Bearer");
        CHECK(body.is_object() && body.size() == 3 && body["model"] == "m-decide", "体里是 model / state / questions");
        CHECK(body["state"]["user"] == "干活" && body["state"]["assistant"] == "MAIN_SECOND",
              "state 是这一趟 user 的话与最后一条助手正文（打回的不算 user）");

        const nlohmann::json h = history(a);
        int kicked = 0;
        for (const auto &m : h)
            if (m.value("role", "") == "user")
                for (const auto &b : m["content"])
                    if (b.value("type", "") == "text" && b.value("text", "") == kKickback) ++kicked;
        CHECK(kicked == 1, "打回那条进了历史，一次打回一条");
        CHECK(std::string(kKickback).rfind(kSupervisorTag, 0) == 0,
              "打回那条带 supervisor 标记，下一次判定认得出它不是用户说的");

        const std::string ended = f.last("agent_end");
        CHECK(has(ended, "RECAP_DONE"), "收工那一帧带 recap");

        const std::string deltas = f.joined("message_update");
        CHECK(has(deltas, "MAIN_FIRST") && has(deltas, "MAIN_SECOND"), "主模型的正文照推");
        CHECK(f.count("message_update") == 2, "recap 那次调用的正文一个字都没推给客户端：只有主模型的两段");
    }

    printf("== System One 一直说没干完：连着三轮没跑工具就强制收工 ==\n");
    {
        Frames f;
        CoreContext ctx{.config = &*cfg,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int main_before = ep.main_calls(), decision_before = ep.decision_calls(), small_before = ep.small_calls();
        ep.main_says({"STILL_TALKING"});
        ep.decision_says({0.1});
        ep.small_says({"RECAP_FORCED"});

        Agent a(ctx, approval, home.string(), 3);
        a.post("干活");
        CHECK(wait_end(f), "还是收工了——不设轮数上限，但推不动了要认");
        CHECK(ep.main_calls() - main_before == 3,
              "主模型跑了三轮就打住：打回两次，第三次判定强制收工（kMaxStall）");
        CHECK(ep.decision_calls() - decision_before == 3, "System One 问了三次");
        CHECK(ep.small_calls() - small_before == 1 && has(f.last("agent_end"), "RECAP_FORCED"),
              "强制收工也是收工：小模型写一次 recap，邻居照样知道这一趟干了什么");
    }

    printf("== 没配 decision_model：不问 System One，没调工具就算干完了 ==\n");
    {
        // **不回落**主模型或小模型（ADR-0010）。也去掉 small_model：什么都不问
        nlohmann::json s = revlm;
        s.erase("decision_model");
        s.erase("small_model");
        auto c2 = reload(home, s);
        if (!c2) return 1;

        Frames f;
        CoreContext ctx{.config = &*c2,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int decision_before = ep.decision_calls(), small_before = ep.small_calls();
        ep.main_says({"ONLY_ANSWER"});

        Agent a(ctx, approval, home.string(), 2);
        a.post("说句话");
        CHECK(wait_end(f), "收工了");
        CHECK(ep.decision_calls() == decision_before, "System One 一次没问");
        CHECK(ep.small_calls() == small_before, "小模型一次没问");
        CHECK(has(f.last("agent_end"), "\"recap\":\"\""), "没有小模型就没有 recap，字段仍在（帧形状恒定）");
    }

    printf("== 端点不是 revlm：decision_model 配了也不问 ==\n");
    {
        nlohmann::json s = revlm;
        s["protocol"] = "openai-chat";
        s["base_url"] = ep.base_url() + "/v1";
        auto c3 = reload(home, s);
        if (!c3) return 1;

        Frames f;
        CoreContext ctx{.config = &*c3,
                        .pricing = nullptr,
                        .emit_fn = [&f](const std::string &t, const std::string &p) { f.add(t, p); }};
        ApprovalCoordinator approval;
        const int decision_before = ep.decision_calls(), small_before = ep.small_calls();
        ep.main_says({"ONLY_ANSWER"});
        ep.small_says({"RECAP_PLAIN"});

        Agent a(ctx, approval, home.string(), 4);
        a.post("说句话");
        CHECK(wait_end(f), "收工了");
        CHECK(ep.decision_calls() == decision_before, "System One 一次没问");
        CHECK(ep.small_calls() - small_before == 1 && has(f.last("agent_end"), "RECAP_PLAIN"), "小模型照写 recap");
    }

    printf(failures ? "\nFAILED (%d)\n" : "\nALL PASS\n", failures);
    fs::remove_all(home);
    return failures ? 1 : 0;
}
