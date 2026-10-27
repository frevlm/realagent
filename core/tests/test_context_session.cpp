/*
 * test_context_session.cpp — 增量上下文协议的客户端（ContextSession，字节级增量）
 *
 * 单元部分不碰网络：
 *   head/tail/middle 重建性质（追加、中间改一处、cache_control 挪位、改 system、
 *   完全相同、P 为空、随机串）；4 KiB 门槛；SHA-256 已知向量；P 只在 X-Revlm-Stored
 *   等于 sha256(B) 时前进；409 判别；统计数字。
 *
 * 端到端部分用本地假 revlm（按 sha256 存请求体、拼回增量、对不认识的 base 回 409
 * revlm_base_miss、回 X-Revlm-Stored），同一个 Agent 跑多趟，中间强制 base 丢失与
 * expect 不符各一次，断言每次都自动以完整体重发并恢复。假 revlm 有 h2c 与 HTTP/1.1
 * 两个前端，各跑一遍：前者走 revlm::net，后者退回 curl。
 */
#include <httplib.h>
#include <support/h2_test_server.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.hpp"
#include "agent/approval.hpp"
#include "agent/context.hpp"
#include "config.hpp"
#include "llm/llm.hpp"

using namespace realagent;
using nlohmann::json;
namespace fs = std::filesystem;

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

/* —— 请求体素材：key 排序的 /v1/messages 形状，messages 在最前，model/system/tools 在后 —— */

struct BodySpec {
    std::vector<std::string> texts;
    int cache_at = -1; // 哪条消息带 cache_control；-1 = 都不带
    std::string system = "You are a coding agent.";
    std::string tools = std::string(6000, 't');
};

static std::string make_body(const BodySpec &s)
{
    std::string out = "{\"messages\":[";
    for (size_t i = 0; i < s.texts.size(); ++i)
    {
        if (i) out += ',';
        out += "{\"content\":[{";
        if (static_cast<int>(i) == s.cache_at) out += "\"cache_control\":{\"type\":\"ephemeral\"},";
        out += "\"text\":\"" + s.texts[i] + "\",\"type\":\"text\"}],\"role\":\"user\"}";
    }
    return out + "],\"model\":\"m\",\"system\":\"" + s.system + "\",\"tools\":\"" + s.tools + "\"}";
}

static std::string rand_text(std::mt19937 &rng, size_t n)
{
    std::string t(n, 'a');
    for (char &c : t) c = static_cast<char>('a' + rng() % 26);
    return t;
}

static BodySpec rand_spec(std::mt19937 &rng)
{
    BodySpec s;
    const size_t n = 1 + rng() % 6;
    for (size_t i = 0; i < n; ++i) s.texts.push_back(rand_text(rng, 1 + rng() % 400));
    s.tools = rand_text(rng, rng() % 3000);
    return s;
}

/* P[0:head] + middle + P[|P|-tail:] == B，且 head + tail <= min(|P|,|B|) */
static bool reconstructs(const std::string &p, const std::string &b)
{
    const ContextSession::Keep k = ContextSession::diff(p, b);
    if (k.head + k.tail > std::min(p.size(), b.size())) return false;
    const std::string middle = b.substr(k.head, b.size() - k.head - k.tail);
    return p.substr(0, k.head) + middle + p.substr(p.size() - k.tail) == b;
}

static void test_reconstruction()
{
    printf("[head/tail/middle 重建]\n");
    std::mt19937 rng(12345);
    int bad = 0, total = 0;
    const auto run = [&](const std::string &p, const std::string &b) {
        ++total;
        if (!reconstructs(p, b)) ++bad;
    };
    for (int i = 0; i < 300; ++i)
    {
        BodySpec p = rand_spec(rng);
        const std::string pb = make_body(p);

        BodySpec app = p; // 追加
        for (size_t k = 0, n = 1 + rng() % 3; k < n; ++k) app.texts.push_back(rand_text(rng, 50));
        run(pb, make_body(app));

        BodySpec ed = p; // 中间改一处
        std::string &t = ed.texts[rng() % ed.texts.size()];
        t[rng() % t.size()] = 'Z';
        run(pb, make_body(ed));

        BodySpec cc = p; // cache_control 挪位
        p.cache_at = static_cast<int>(rng() % p.texts.size());
        cc.cache_at = static_cast<int>(rng() % cc.texts.size());
        run(make_body(p), make_body(cc));

        BodySpec sys = p; // 改 system
        sys.system += rand_text(rng, 1 + rng() % 40);
        run(pb, make_body(sys));

        run(pb, pb);                                                     // 完全相同
        run("", pb);                                                     // P 为空
        run(pb, "");                                                     // B 为空
        run(rand_text(rng, rng() % 200), rand_text(rng, rng() % 200));   // 随机串
        run(std::string(rng() % 50, 'x'), std::string(rng() % 50, 'x')); // 重复字节：head/tail 抢同一段
    }
    CHECK(total == 300 * 9 && bad == 0, "2700 组 (P,B) 全部满足重建 B == P[0:head]+middle+P[|P|-tail:]");

    const std::string p = make_body({{"hello"}});
    BodySpec two{{"hello", "again"}};
    const ContextSession::Keep k = ContextSession::diff(p, make_body(two));
    CHECK(k.head == p.find("],\"model\"") && k.tail == p.size() - p.find("],\"model\""),
          "追加一条消息：head 到旧 messages 数组闭合之前，tail 是其后的全部字节");
    const ContextSession::Keep same = ContextSession::diff(p, p);
    CHECK(same.head == p.size() && same.tail == 0, "完全相同：head = |P|，tail = 0");
}

static void test_sha256()
{
    printf("[SHA-256]\n");
    CHECK(ContextSession::sha256_hex("abc") ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "abc 的已知向量");
    CHECK(ContextSession::sha256_hex("") ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "空串的已知向量");
}

/* 发送体里的某个协议头；没有 → 空串 */
static std::string header_of(const ContextSession::Wire &w, const std::string &name)
{
    for (const std::string &h : w.headers)
        if (h.starts_with(name + ": ")) return h.substr(name.size() + 2);
    return "";
}

/* 门槛：P = X + "a" + Y，B = X + "b" + Y，省下 |X|+|Y| 字节 */
static ContextSession::Wire send_pair(size_t saved)
{
    ContextSession cs;
    const std::string x(saved / 2, 'x'), y(saved - saved / 2, 'y');
    const std::string p = x + "a" + y;
    cs.prepare(p);
    cs.on_response(ContextSession::sha256_hex(p));
    return cs.prepare(x + "b" + y);
}

static void test_threshold()
{
    printf("[4 KiB 门槛]\n");
    const ContextSession::Wire below = send_pair(4095);
    CHECK(header_of(below, "X-Revlm-Base").empty() && header_of(below, "X-Revlm-Keep").empty() &&
              !header_of(below, "X-Revlm-Expect").empty(),
          "省 4095 字节：完整模式，只带 Expect");
    CHECK(below.body.size() == 4096, "完整模式的请求体是整个 B（4095 + 1 字节）");

    const ContextSession::Wire at = send_pair(4096);
    CHECK(header_of(at, "X-Revlm-Keep") == "2048,2048", "省 4096 字节：增量模式，Keep = head,tail");
    CHECK(at.body == "b", "增量体只有 middle");
    CHECK(header_of(at, "X-Revlm-Base") == ContextSession::sha256_hex(std::string(2048, 'x') + "a" +
                                                                      std::string(2048, 'y')),
          "Base 是 P 的 SHA-256");
    CHECK(header_of(at, "X-Revlm-Expect") ==
              ContextSession::sha256_hex(std::string(2048, 'x') + "b" + std::string(2048, 'y')),
          "Expect 是完整 B 的 SHA-256");
}

static void test_base_advances_only_on_stored()
{
    printf("[P 只在 X-Revlm-Stored 匹配时前进]\n");
    const std::string head(5000, 'h');
    const auto body = [&](const std::string &mid) { return head + mid + "|tail"; };
    ContextSession cs;

    cs.prepare(body("1"));
    cs.on_response(""); // 没带头：没存下
    ContextSession::Wire w = cs.prepare(body("2"));
    CHECK(header_of(w, "X-Revlm-Base").empty(), "响应没带 Stored：下一轮仍是完整体");

    cs.on_response(ContextSession::sha256_hex(body("other"))); // 别的请求体的 id
    w = cs.prepare(body("3"));
    CHECK(header_of(w, "X-Revlm-Base").empty(), "Stored 与 sha256(B) 不符：下一轮仍是完整体");

    cs.on_response(ContextSession::sha256_hex(body("3")));
    w = cs.prepare(body("4"));
    CHECK(header_of(w, "X-Revlm-Base") == ContextSession::sha256_hex(body("3")),
          "Stored 匹配：B 成为 base");
    CHECK(w.body == "4", "增量体是 middle");

    // 409 之后重发完整体；重发成功存下的是这一份
    w = cs.full();
    CHECK(header_of(w, "X-Revlm-Base").empty() && w.body == body("4"), "full()：完整模式、完整体");
    cs.on_response(ContextSession::sha256_hex(body("4")));
    w = cs.prepare(body("5"));
    CHECK(header_of(w, "X-Revlm-Base") == ContextSession::sha256_hex(body("4")),
          "重发被存下后，下一轮以它为 base");
}

static void test_conflict_detection()
{
    printf("[409 判别]\n");
    const auto err = [](const char *type) {
        return std::string(R"({"type":"error","error":{"type":")") + type + R"(","message":"x"}})";
    };
    CHECK(ContextSession::is_conflict(409, err("revlm_base_miss")), "409 revlm_base_miss");
    CHECK(ContextSession::is_conflict(409, err("revlm_expect_mismatch")), "409 revlm_expect_mismatch");
    CHECK(ContextSession::is_conflict(409, err("something_else")) == false, "其它 409 是真错误");
    CHECK(ContextSession::is_conflict(409, "not json") == false, "409 但错误体不是 JSON");
    CHECK(ContextSession::is_conflict(400, err("revlm_base_miss")) == false, "非 409 不算");
}

static void test_stats()
{
    printf("[统计]\n");
    const std::string head(6000, 'h');
    const auto body = [&](const std::string &mid) { return head + mid + "|tail"; };
    ContextSession cs;

    cs.prepare(body("1"));
    cs.on_response(ContextSession::sha256_hex(body("1")));
    const ContextSession::Wire w = cs.prepare(body("22"));
    cs.on_response(ContextSession::sha256_hex(body("22")));
    ContextSession::Stats s = cs.stats();
    CHECK(s.requests == 2 && s.deltas == 1 && s.hits == 1 && s.misses == 0, "一次完整、一次增量命中");
    CHECK(s.bytes_saved == static_cast<long long>(body("22").size() - w.body.size()),
          "bytes_saved = |B| - |实际发送体|");

    cs.prepare(body("333"));
    cs.full();
    cs.on_response(ContextSession::sha256_hex(body("333")));
    s = cs.stats();
    CHECK(s.requests == 3 && s.deltas == 2 && s.hits == 1 && s.misses == 1,
          "增量被 409：记一次 miss，重发成功不落命中");
}

static Config load_config(const std::string &protocol, const std::string &base_url)
{
    const fs::path home = fs::temp_directory_path() / ("realagent_ctxsess_home_" + protocol);
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    json s{{"protocol", protocol}, {"base_url", base_url}, {"api_key", "k"}, {"model", "m-test"}, {"permission", "allow-all"}, {"revlm_context", true}};
    std::ofstream(home / ".realagent" / "settings.json") << s.dump();
    ::setenv("HOME", home.c_str(), 1);
    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: Config::load: %s\n", cfg.error().c_str());
        std::exit(1);
    }
    return std::move(*cfg);
}

static void test_protocols_enabled()
{
    printf("[三种协议都启用]\n");
    for (const char *p : {"anthropic-messages", "openai-chat", "openai-responses"})
    {
        const Config cfg = load_config(p, "http://127.0.0.1:9");
        CHECK(context_protocol_enabled(cfg), p);
    }
}

/* —— 端到端：httplib 假 revlm —— */

static std::string sse_text(const std::string &text)
{
    const json start{{"type", "content_block_start"},
                     {"index", 0},
                     {"content_block", {{"type", "text"}, {"text", ""}}}};
    const json delta{{"type", "content_block_delta"},
                     {"index", 0},
                     {"delta", {{"type", "text_delta"}, {"text", text}}}};
    const json stop{{"type", "message_delta"}, {"delta", {{"stop_reason", "end_turn"}}}};
    return "event: content_block_start\ndata: " + start.dump() + "\n\n" +
           "event: content_block_delta\ndata: " + delta.dump() + "\n\n" +
           "event: message_delta\ndata: " + stop.dump() + "\n\n";
}

static std::string sse_tool_use()
{
    const json start{{"type", "content_block_start"},
                     {"index", 0},
                     {"content_block",
                      {{"type", "tool_use"}, {"id", "t1"}, {"name", "read"}, {"input", json::object()}}}};
    const json delta{{"type", "content_block_delta"},
                     {"index", 0},
                     {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"file_path\":\"nope.txt\"}"}}}};
    const json blk_stop{{"type", "content_block_stop"}, {"index", 0}};
    const json stop{{"type", "message_delta"}, {"delta", {{"stop_reason", "tool_use"}}}};
    return "event: content_block_start\ndata: " + start.dump() + "\n\n" +
           "event: content_block_delta\ndata: " + delta.dump() + "\n\n" +
           "event: content_block_stop\ndata: " + blk_stop.dump() + "\n\n" +
           "event: message_delta\ndata: " + stop.dump() + "\n\n";
}

/* 假 revlm：按 sha256 存请求体；增量请求按 Base/Keep 拼回，base 不认识 → 409
 * revlm_base_miss，拼出的字节与 Expect 不符 → 409 revlm_expect_mismatch；成功后回
 * X-Revlm-Stored。第一个请求回 tool_use，其余回文本。协议逻辑与传输无关，两个前端：
 * H2FakeRevlm（h2c，realagent 走 revlm::net）与 H1FakeRevlm（只认 HTTP/1.1，realagent
 * 退回 curl）。 */
class FakeRevlm {
  public:
    struct Seen {
        bool delta = false;
        int status = 0;
        std::string body; // 拼回后的完整体（拼不出来 = 空）
    };

    virtual ~FakeRevlm() = default;
    virtual std::string base_url() const = 0;

    std::vector<Seen> seen() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return seen_;
    }
    void forget_all() // 模拟存储被清理 / 换了实例
    {
        std::lock_guard<std::mutex> lk(mtx_);
        store_.clear();
    }
    void corrupt_next_delta() // 下一个增量请求的拼装结果与 Expect 对不上
    {
        std::lock_guard<std::mutex> lk(mtx_);
        corrupt_ = true;
    }

  protected:
    struct Reply {
        int status = 200;
        std::string content_type;
        std::string body;
        std::string stored; // X-Revlm-Stored；空 = 不带
    };

    /* header(name) 取请求头，没有就是空串。 */
    Reply serve(const std::function<std::string(const char *)> &header, const std::string &body)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Reply rp;
        const Seen seen = handle(header, body, rp);
        seen_.push_back(seen);
        if (rp.status != 200) return rp;
        rp.content_type = "text/event-stream";
        rp.stored = ContextSession::sha256_hex(seen.body);
        rp.body = served_++ == 0 ? sse_tool_use() : sse_text("REPLY");
        return rp;
    }

  private:
    static void conflict(Reply &rp, const char *type)
    {
        rp.status = 409;
        rp.content_type = "application/json";
        rp.body = std::string(R"({"type":"error","error":{"type":")") + type + R"(","message":"x"}})";
    }

    Seen handle(const std::function<std::string(const char *)> &header, const std::string &body, Reply &rp)
    {
        Seen seen;
        seen.delta = !header("X-Revlm-Base").empty();
        std::string full = body;
        if (seen.delta)
        {
            const auto base = store_.find(header("X-Revlm-Base"));
            size_t head = 0, tail = 0;
            if (sscanf(header("X-Revlm-Keep").c_str(), "%zu,%zu", &head, &tail) != 2 ||
                base == store_.end() || head + tail > base->second.size())
            {
                seen.status = 409;
                conflict(rp, "revlm_base_miss");
                return seen;
            }
            const std::string &p = base->second;
            full = p.substr(0, head) + body + p.substr(p.size() - tail);
            if (std::exchange(corrupt_, false)) full += "!";
        }
        if (ContextSession::sha256_hex(full) != header("X-Revlm-Expect"))
        {
            seen.status = 409;
            conflict(rp, "revlm_expect_mismatch");
            return seen;
        }
        store_[ContextSession::sha256_hex(full)] = full;
        seen.status = rp.status = 200;
        seen.body = full;
        return seen;
    }

    mutable std::mutex mtx_;
    std::map<std::string, std::string> store_;
    std::vector<Seen> seen_;
    int served_ = 0;
    bool corrupt_ = false;
};

class H2FakeRevlm : public FakeRevlm {
  public:
    H2FakeRevlm()
        : svr_([this](const revlm::net::test::H2TestRequest &rq) {
              const auto header = [&rq](const char *name) {
                  std::string want(name);
                  for (char &c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                  for (const auto &h : rq.headers)
                      if (h.name == want) return h.value;
                  return std::string();
              };
              const Reply rp = serve(header, rq.body);
              revlm::net::test::H2TestResponse rs;
              rs.status = rp.status;
              rs.headers.push_back({"content-type", rp.content_type});
              if (!rp.stored.empty()) rs.headers.push_back({"x-revlm-stored", rp.stored});
              rs.body = rp.body;
              return rs;
          })
    {
    }
    std::string base_url() const override { return "http://127.0.0.1:" + std::to_string(svr_.port()); }

  private:
    revlm::net::test::H2TestServer svr_;
};

class H1FakeRevlm : public FakeRevlm {
  public:
    H1FakeRevlm()
    {
        svr_.Post("/v1/messages", [this](const httplib::Request &rq, httplib::Response &rs) {
            const Reply rp = serve([&rq](const char *name) { return rq.get_header_value(name); }, rq.body);
            rs.status = rp.status;
            if (!rp.stored.empty()) rs.set_header("X-Revlm-Stored", rp.stored);
            rs.set_content(rp.body, rp.content_type);
        });
        port_ = svr_.bind_to_any_port("127.0.0.1");
        th_ = std::thread([this] { svr_.listen_after_bind(); });
    }
    ~H1FakeRevlm() override
    {
        svr_.stop();
        if (th_.joinable()) th_.join();
    }
    std::string base_url() const override { return "http://127.0.0.1:" + std::to_string(port_); }

  private:
    httplib::Server svr_;
    int port_ = 0;
    std::thread th_;
};

/* 收到的帧（等 agent_end 用） */
struct Frames {
    mutable std::mutex mtx;
    std::vector<std::string> v;
    void add(const std::string &t)
    {
        std::lock_guard<std::mutex> lk(mtx);
        v.push_back(t);
    }
    int count(const std::string &t) const
    {
        std::lock_guard<std::mutex> lk(mtx);
        int n = 0;
        for (const auto &e : v)
            if (e == t) ++n;
        return n;
    }
};

static bool wait_end(const Frames &f, int n)
{
    for (int i = 0; i < 600; ++i)
    {
        if (f.count("agent_end") >= n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/* 一趟 post 之后，假服务端新增的请求 */
static std::vector<FakeRevlm::Seen> since(const FakeRevlm &ep, size_t from)
{
    const std::vector<FakeRevlm::Seen> all = ep.seen();
    return {all.begin() + std::min(from, all.size()), all.end()};
}

static void test_e2e(FakeRevlm &ep)
{
    Config cfg = load_config("anthropic-messages", ep.base_url());
    const std::string workdir = (fs::temp_directory_path() / "realagent-ctx-e2e").string();
    fs::create_directories(workdir);

    Frames f;
    CoreContext ctx{.config = &cfg,
                    .pricing = nullptr,
                    .emit_fn = [&f](const std::string &t, const std::string &) { f.add(t); }};
    ApprovalCoordinator approval;
    Agent a(ctx, approval, workdir, 1);

    /* 第 1 趟：tool_use → 工具结果 → 文本，两次调用：完整体、增量命中 */
    a.post("干活");
    CHECK(wait_end(f, 1), "第 1 趟收工");
    std::vector<FakeRevlm::Seen> s = since(ep, 0);
    CHECK(s.size() == 2 && !s[0].delta && s[0].status == 200 && s[1].delta && s[1].status == 200,
          "第 1 趟：完整体 → 增量（200）");
    CHECK(a.context_session().stats().hits == 1 && a.context_session().stats().bytes_saved > 0,
          "增量命中，节省字节 > 0");

    /* 第 2 趟：存储被清掉 → 增量 409 revlm_base_miss → 完整重发 */
    size_t n = ep.seen().size();
    ep.forget_all();
    a.post("再来");
    CHECK(wait_end(f, 2), "第 2 趟收工（base 丢失后恢复）");
    s = since(ep, n);
    CHECK(s.size() == 2 && s[0].delta && s[0].status == 409 && !s[1].delta && s[1].status == 200,
          "第 2 趟：增量 409 → 完整重发 200");
    CHECK(a.context_session().stats().misses == 1, "统计：一次 miss");

    /* 第 3 趟：重发的完整体被存下，这一趟增量命中 */
    n = ep.seen().size();
    a.post("三");
    CHECK(wait_end(f, 3), "第 3 趟收工");
    s = since(ep, n);
    CHECK(s.size() == 1 && s[0].delta && s[0].status == 200, "第 3 趟：增量命中（重发已被存下）");

    /* 第 4 趟：拼装结果与 Expect 不符 → 409 revlm_expect_mismatch → 完整重发 */
    n = ep.seen().size();
    ep.corrupt_next_delta();
    a.post("四");
    CHECK(wait_end(f, 4), "第 4 趟收工（expect 不符后恢复）");
    s = since(ep, n);
    CHECK(s.size() == 2 && s[0].delta && s[0].status == 409 && !s[1].delta && s[1].status == 200,
          "第 4 趟：增量 409 → 完整重发 200");

    const ContextSession::Stats st = a.context_session().stats();
    CHECK(st.requests == 5 && st.deltas == 4 && st.hits == 2 && st.misses == 2,
          "统计：5 个请求、4 个增量、2 次命中、2 次 miss");
    fs::remove_all(workdir);
}

int main()
{
    test_reconstruction();
    test_sha256();
    test_threshold();
    test_base_advances_only_on_stored();
    test_conflict_detection();
    test_stats();
    test_protocols_enabled();
    {
        printf("[端到端：h2 假 revlm，走 revlm::net]\n");
        H2FakeRevlm ep;
        test_e2e(ep);
    }
    {
        printf("[端到端：只认 HTTP/1.1 的假 revlm，退回 curl]\n");
        H1FakeRevlm ep;
        test_e2e(ep);
    }
    printf(failures ? "\n%d 项失败\n" : "\n全部通过\n", failures);
    return failures ? 1 : 0;
}
