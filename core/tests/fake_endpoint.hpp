/*
 * fake_endpoint.hpp — 端到端测试的假端点与收帧器（test_stop_loop、test_compact 共用）
 *
 * 一个 HTTP/1.1 桩，同时扮 revlm 的 /v1/models、/v1/systemone 与聊天（按请求里的模型名挑一份预置的
 * SSE 回）。send_revlm 先说 h2，桩回 HTTP/1.1，于是退回 curl（transport.hpp），整条路走得通，不必真花钱。
 */
#pragma once

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
#include "agent/session.hpp"
#include "config.hpp"

static bool has(const std::string &hay, const std::string &needle)
{
    return hay.find(needle) != std::string::npos;
}

/* —— 假端点：一个线程、一个监听套接字、按路径与模型名挑回答 ——
 *
 * 聊天说 openai-chat；m-claude 说 anthropic-messages，回一个只有签名的思考块（看它落不落盘）。
 * 这里验的是 loop 的出口，不是协议解析（那是 test_llm 的活）。 */
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

    /* 主模型这样回答就是 HTTP 400「上下文超长」 */
    static constexpr const char *kOverflow = "<overflow>";

    /* 主模型每被问一次，就按顺序发下一份回答（问多了就重复最后一份）。换一份就从头发 */
    void main_says(std::vector<std::string> texts)
    {
        main_ = std::move(texts);
        main_i_ = 0;
    }
    /* 主模型每次回答都报这么多提示词 token（0 = 不报 usage） */
    void main_prompt_tokens(long long n) { prompt_tokens_ = n; }
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

    /* m-claude 最近一次请求的体 */
    std::string last_claude_request() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return last_claude_;
    }

    /* 主模型收到过的请求体，按到达顺序 */
    std::vector<std::string> main_requests() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return main_bodies_;
    }

  private:
    /* anthropic-messages 的一次回答：思考块正文为空、只有块尾签名（新模型默认 display: omitted），再一段正文 */
    static inline const std::string kOmittedThinking =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":1}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIG_OMITTED\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"CLAUDE_ANSWER\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":3}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

    static std::string sse(const std::string &text, long long prompt_tokens)
    {
        nlohmann::json d{{"choices", nlohmann::json::array({{{"delta", {{"content", text}}}}})}};
        nlohmann::json fin{
            {"choices", nlohmann::json::array({{{"delta", nlohmann::json::object()}, {"finish_reason", "stop"}}})}};
        std::string out = "data: " + d.dump() + "\n\ndata: " + fin.dump() + "\n\n";
        if (prompt_tokens > 0)
            out += "data: " +
                   nlohmann::json{{"choices", nlohmann::json::array()},
                                  {"usage", {{"prompt_tokens", prompt_tokens}, {"completion_tokens", 1}}}}
                       .dump() +
                   "\n\n";
        return out + "data: [DONE]\n\n";
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
            data.push_back({{"id", "m-claude"}, {"protocol", "messages"}});
            return json_reply({{"data", data}});
        }
        if (has(req, "m-claude"))
        {
            std::lock_guard<std::mutex> lk(mtx_);
            last_claude_ = req.substr(req.find("\r\n\r\n") + 4);
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" + kOmittedThinking;
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
        if (has(req, "m-small"))
            return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" +
                   sse(pick(small_, small_n_++), 0);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            main_bodies_.push_back(req.substr(req.find("\r\n\r\n") + 4));
        }
        ++main_n_;
        const std::string text = pick(main_, main_i_++);
        if (text == kOverflow)
        {
            const std::string body = R"({"error":{"message":"prompt is too long: 300000 tokens > 200000 maximum"}})";
            return "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: " +
                   std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        }
        return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n" +
               sse(text, prompt_tokens_);
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
    std::atomic<int> main_i_{0}; // 这一份回答发到第几条
    std::atomic<long long> prompt_tokens_{0};
    mutable std::mutex mtx_;
    std::string last_decision_, last_claude_;
    std::vector<std::string> main_bodies_;
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

static nlohmann::json history(const realagent::Agent &a)
{
    nlohmann::json out = nlohmann::json::array();
    realagent::Session::read(a.session_dir(), a.session_id(), out);
    return out;
}

/* settings.json 换一份并重载 */
static std::optional<realagent::Config> reload(const std::filesystem::path &home, const nlohmann::json &s)
{
    std::ofstream(home / ".realagent" / "settings.json") << s.dump();
    auto c = realagent::Config::load();
    if (!c)
    {
        printf("  FAIL: 配置加载失败\n");
        return std::nullopt;
    }
    return std::move(*c);
}
