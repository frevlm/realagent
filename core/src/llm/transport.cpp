/*
 * transport.cpp — send_curl 与 send_revlm（见 transport.hpp）
 */
#include "llm/transport.hpp"

#include <curl/curl.h>

#include <net/http2.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <future>
#include <system_error>
#include <thread>

namespace realagent {

namespace net = revlm::net;
using namespace std::chrono_literals;

namespace {

/* 非 2xx 的响应体不是 SSE，留着报错用；2xx 交给 sink。false = sink 要求中止。 */
bool deliver(SendResult &r, const BodySink &sink, std::string_view chunk)
{
    if (r.status >= 400)
    {
        if (r.error_body.size() < 8192) r.error_body.append(chunk);
        return true;
    }
    if (sink(chunk)) return true;
    r.sink_failed = true;
    return false;
}

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

/* —— curl —— */

struct CurlCtx {
    CURL *curl;
    SendResult *r;
    const BodySink *sink;
};

int curl_progress_cb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return static_cast<const std::atomic<bool> *>(clientp)->load() ? 1 : 0;
}

size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *c = static_cast<CurlCtx *>(userdata);
    const size_t n = size * nmemb;
    if (c->r->status == 0) curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &c->r->status);
    return deliver(*c->r, *c->sink, std::string_view(ptr, n)) ? n : 0; // 返回 < n 让 curl 中止传输
}

/* 响应头里只捞 X-Revlm-Stored。 */
size_t curl_header_cb(char *buf, size_t size, size_t nmemb, void *userdata)
{
    std::string_view line(buf, size * nmemb);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);
    const std::string_view name = "x-revlm-stored:";
    if (line.size() > name.size() && iequals(line.substr(0, name.size()), name))
    {
        std::string_view value = line.substr(name.size());
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        static_cast<CurlCtx *>(userdata)->r->revlm_stored = std::string(value);
    }
    return size * nmemb;
}

/* —— revlm::net —— */

/* net 的等待超时只让这次等待返回、不毁流：每 kPoll 醒一次看用户中断。 */
constexpr auto kPoll = 200ms;

bool timed_out(const std::system_error &e) { return e.code() == std::errc::timed_out; }

/* "Key: value" → h2 头（名字小写）。 */
net::Headers net_headers(const std::vector<std::string> &lines)
{
    net::Headers out;
    for (const std::string &line : lines)
    {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (char &ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        std::string_view value = std::string_view(line).substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        out.push_back({std::move(name), std::string(value)});
    }
    return out;
}

struct NetCall {
    const HttpRequest &req;
    const BodySink &sink;
    const std::atomic<bool> &abort;
    SendResult r;
    bool not_h2 = false; // 对面不说 h2 / realurl/1：请求没被当成请求处理过，可以换条路重发
    std::promise<void> done;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();

    /* 该收手了就给出原因，否则空 */
    const char *stop() const
    {
        if (abort.load()) return "已中断";
        if (std::chrono::steady_clock::now() >= deadline) return "超时";
        return nullptr;
    }
};

net::Task<void> exchange(net::Client &client, NetCall &c)
{
    net::Stream st;
    try
    {
        st = co_await client.open(c.req.method, c.req.url, net_headers(c.req.headers));
    } catch (const std::exception &e)
    {
        c.not_h2 = true;
        c.r.error = e.what();
        co_return;
    }

    net::ResponseHead head;
    try
    {
        if (!c.req.body.empty()) co_await st.write(c.req.body);
        co_await st.end();
        while (!c.stop())
        {
            try
            {
                head = co_await st.head(kPoll);
                break;
            } catch (const std::system_error &e)
            {
                if (!timed_out(e)) throw;
            }
        }
    } catch (const std::system_error &e)
    {
        // h2c 建连不等对面的 SETTINGS：HTTP/1.1 服务端回的第一帧就不合 h2，错在响应头之前
        if (e.code() != std::errc::protocol_error) throw;
        c.not_h2 = true;
        c.r.error = e.what();
        co_return;
    }
    if (head.status == 0) // 等响应头时被中断或超时
    {
        st.cancel();
        c.r.error = c.stop();
        co_return;
    }
    c.r.status = head.status;
    for (const net::Header &h : head.headers)
        if (iequals(h.name, "x-revlm-stored")) c.r.revlm_stored = h.value;

    std::string buf(16384, '\0');
    for (;;)
    {
        if (const char *why = c.stop())
        {
            st.cancel();
            c.r.error = why;
            co_return;
        }
        std::size_t n = 0;
        try
        {
            n = co_await st.read(buf, kPoll);
        } catch (const std::system_error &e)
        {
            if (!timed_out(e)) throw;
            continue;
        }
        if (n == 0) co_return;
        if (!deliver(c.r, c.sink, std::string_view(buf.data(), n)))
        {
            st.cancel();
            co_return;
        }
    }
}

net::Task<void> run(net::Client &client, NetCall &c)
{
    try
    {
        co_await exchange(client, c);
    } catch (const std::exception &e)
    {
        c.r.error = std::string("revlm 连接出错: ") + e.what();
    }
    c.done.set_value();
}

/* 进程里一条 net 线程：一个 EventLoop + 一个 Client，所有 agent 的 revlm 调用都多路复用在
 * 它的连接上。loop 一直在跑，空闲连接的 PING / GOAWAY / 断开都有人处理。 */
class Link {
  public:
    Link()
    {
        std::promise<void> ready;
        th_ = std::thread([this, &ready] {
            net::EventLoop loop;
            net::Client client(loop);
            loop_ = &loop;
            client_ = &client;
            ready.set_value();
            loop.run();
        });
        ready.get_future().wait();
    }
    ~Link()
    {
        loop_->post([l = loop_] { l->stop(); });
        th_.join();
    }

    /* sink 在 net 线程上被调用；调用方一直阻塞到这次调用结束。 */
    void call(NetCall &c)
    {
        std::future<void> done = c.done.get_future();
        loop_->post([this, &c] { loop_->spawn(run(*client_, c)); });
        done.wait();
    }

  private:
    std::thread th_;
    net::EventLoop *loop_ = nullptr;
    net::Client *client_ = nullptr;
};

} // namespace

SendResult send_curl(const HttpRequest &req, const BodySink &sink, const std::atomic<bool> &abort)
{
    SendResult r;
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        r.error = "curl 初始化失败";
        return r;
    }
    CurlCtx c{curl, &r, &sink};
    curl_slist *hdrs = nullptr;
    for (const auto &h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    if (req.method == "POST") curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_s);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &c);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &c);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    curl_slist_free_all(hdrs);
    if (rc != CURLE_OK) r.error = std::string("curl 失败: ") + curl_easy_strerror(rc);
    return r;
}

SendResult send_revlm(const HttpRequest &req, const BodySink &sink, const std::atomic<bool> &abort)
{
    static Link link;
    NetCall c{.req = req, .sink = sink, .abort = abort};
    if (req.timeout_s > 0) c.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(req.timeout_s);
    link.call(c);
    if (c.not_h2)
    {
        fprintf(stderr, "[transport] revlm::net 跟 %s 说不上 h2（%s），本次退回 curl\n", req.url.c_str(),
                c.r.error.c_str());
        return send_curl(req, sink, abort);
    }
    return std::move(c.r);
}

} // namespace realagent
