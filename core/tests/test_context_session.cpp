/*
 * test_context_session.cpp — 增量上下文协议的客户端（ContextSession）
 *
 * 单元部分不碰网络：
 *   完整请求 → 增量请求体（revlm_context 字段、messages 只带尾部、expect 与
 *   prefix/count/shape 全部与服务端同一套共享模块独立复算比对）；从增量体 +
 *   已知 blobs 反拼，与客户端本地完整体逐字节一致；回退与 shape 变化退完整请求；
 *   409 判别；统计数字。
 *
 * 端到端部分用本地 httplib 假 revlm：第一次对增量请求回 409（miss / mismatch 两条
 * 路），断言自动重发的是完整体；随后一轮命中，且服务端回一个错的 prefix 头时
 * 客户端以本地为准继续。
 */
#include <httplib.h>

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
#include "config.hpp"
#include "llm/llm.hpp"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

using namespace realagent;
using nlohmann::json;
namespace fs = std::filesystem;
namespace rllm = realurl::llm;

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

/* —— 配置：HOME 指到临时目录，revlm_context 开 —— */
static Config make_config()
{
    const std::string home = (fs::temp_directory_path() / "realagent_ctxsess_home").string();
    fs::remove_all(home);
    fs::create_directories(home + "/.realagent");
    {
        std::ofstream f(home + "/.realagent/settings.json");
        f << R"({"protocol":"anthropic-messages","base_url":"http://127.0.0.1:9","api_key":"k",)"
             R"("model":"m-test","permission":"allow-all","revlm_context":true})";
    }
    setenv("HOME", home.c_str(), 1);
    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: Config::load: %s\n", cfg.error().c_str());
        std::exit(1);
    }
    CHECK(context_protocol_enabled(*cfg), "开关打开且协议是 anthropic-messages → 启用");
    return std::move(*cfg);
}

/* —— 对话素材（抽象形状） —— */
static json user_msg(const std::string &text)
{
    return json{{"role", "user"}, {"content", json::array({json{{"type", "text"}, {"text", text}}})}};
}

static const json kAssistant1 = json::parse(R"({
  "role": "assistant",
  "content": [
    {"type":"thinking","thinking":"先分析问题","signature":"sig-abc"},
    {"type":"text","text":"world"}
  ]})");

static json dialog1(const std::string &system, bool with_tools)
{
    json d{{"model", "m-test"}, {"system", system}, {"messages", json::array({user_msg("hello")})}};
    if (with_tools)
        d["tools"] = json::parse(
            R"([{"name":"read","description":"读文件","input_schema":{"type":"object"}}])");
    return d;
}

static json dialog3(const std::string &system, bool with_tools)
{
    json d = dialog1(system, with_tools);
    d["messages"].push_back(kAssistant1);
    d["messages"].push_back(user_msg("second"));
    return d;
}

/* —— 与服务端同一套共享模块的独立复算 —— */

/* 完整请求体文本 → 拼装体（剥 revlm_context / cache_control → blobs → assemble）。
 * move_shape_to_end = 增量轮的键序规则：system/tools 挪到对象末尾再拼装，
 * 与服务端按 shape 注入的追加位置一致（P3）。 */
static std::string assemble_full(const std::string &body_text,
                                 std::vector<rllm::RequestMark> *marks_out = nullptr,
                                 std::vector<std::string> *blobs_out = nullptr,
                                 bool move_shape_to_end = false)
{
    boost::json::value request = boost::json::parse(body_text);
    boost::json::object &ro = request.as_object();
    ro.erase("revlm_context");
    std::vector<rllm::RequestMark> marks;
    for (const rllm::CacheMark &m : rllm::strip_cache_control(request))
        marks.push_back({m.path, m.value});
    std::vector<std::string> blobs;
    for (const auto &m : ro.at("messages").as_array())
        blobs.push_back(rllm::canonical_message(m));
    if (move_shape_to_end)
    {
        // 重建实现，与客户端同一规则；erase 后 operator[] 是否在末尾是实现细节
        boost::json::object rebuilt;
        std::optional<boost::json::value> system, tools;
        for (const auto &kv : ro)
        {
            if (kv.key() == "system")
            {
                system = kv.value();
                continue;
            }
            if (kv.key() == "tools")
            {
                tools = kv.value();
                continue;
            }
            rebuilt.emplace(kv.key(), kv.value());
        }
        if (system) rebuilt.emplace("system", std::move(*system));
        if (tools) rebuilt.emplace("tools", std::move(*tools));
        request = boost::json::value(std::move(rebuilt));
    }
    if (marks_out) *marks_out = marks;
    if (blobs_out) *blobs_out = blobs;
    return rllm::assemble_request_body(request, blobs, marks);
}

/* revlm_context.cache_control 的 JSON → marks（服务端收到后就是这么解的） */
static std::vector<rllm::RequestMark> marks_from_json(const boost::json::value &v)
{
    std::vector<rllm::RequestMark> out;
    for (const auto &e : v.as_array())
    {
        rllm::RequestMark m;
        for (const auto &el : e.as_object().at("path").as_array())
        {
            if (el.is_string())
                m.path.push_back(std::string(el.as_string().c_str()));
            else
                m.path.push_back(el.to_number<std::uint64_t>());
        }
        m.value = e.as_object().at("value");
        out.push_back(std::move(m));
    }
    return out;
}

/* 协议形状的 assistant 消息（与 record_assistant 内部同一条转换） */
static boost::json::value protocol_assistant(const json &abstract)
{
    return boost::json::parse(
        json{{"role", "assistant"}, {"content", anthropic_content_blocks(abstract)}}.dump());
}

/* —— 单元：完整 → 增量 → 反拼 —— */
static void test_full_then_incremental()
{
    printf("[完整 → 增量 → 反拼]\n");
    const Config cfg = make_config();
    ContextSession cs;

    const std::string r1 = build_request(cfg, dialog1("You are a coding agent.", true)).body;
    const std::string wire1 = cs.wrap_request(r1);
    const json w1 = json::parse(wire1);
    CHECK(w1["revlm_context"].is_object() && w1["revlm_context"].empty(),
          "首轮 revlm_context:{}（客户端还不知道任何前缀）");
    CHECK(w1["messages"].size() == 1, "首轮 messages 带全部消息");

    cs.record_assistant(kAssistant1);

    const std::string r2 =
        build_request(cfg, dialog3("You are a coding agent.", true)).body;
    // dialog3 的第二条 user 是"second"，而历史只到 assistant —— 直接在请求 2 里
    // 验证它确实是三条消息的完整体
    CHECK(json::parse(r2)["messages"].size() == 3, "请求 2 是三消息完整体");

    const std::string wire2 = cs.wrap_request(r2);
    const json w2 = json::parse(wire2);
    CHECK(w2["messages"].size() == 1, "增量 messages 只带尾部");
    CHECK(!w2.contains("system") && !w2.contains("tools"),
          "P3：增量体省略 system/tools（shape 引用，服务端取回注入）");
    CHECK(w2["messages"][0]["role"] == "user" &&
              w2["messages"][0]["content"][0]["text"] == "second",
          "尾部是第 k+1 条起的新消息");
    const json &ctx2 = w2["revlm_context"];
    CHECK(ctx2["count"] == 2, "count = 已知前缀深度（u1 + assistant）");

    /* prefix：本地按同一套链独立复算 node(2) */
    boost::json::value u1 = boost::json::parse(r1).as_object().at("messages").as_array()[0];
    rllm::strip_cache_control(u1);
    rllm::Node n = rllm::chain_node(rllm::root_node(), rllm::blob_hash(u1));
    n = rllm::chain_node(n, rllm::blob_hash(protocol_assistant(kAssistant1)));
    CHECK(ctx2["prefix"] == rllm::to_hex(n), "prefix = 本地复算的 node(2)（KAT：与服务端同函数）");

    /* expect 与 shape：对完整请求的独立拼装 */
    std::vector<std::string> blobs2;
    const std::string full2 = assemble_full(r2, nullptr, &blobs2, /*move_shape_to_end=*/true);
    CHECK(ctx2["expect"] == rllm::sha256_hex(full2),
          "expect = 完整拼装体的 SHA-256（不是增量体的）");
    boost::json::value R2 = boost::json::parse(r2);
    CHECK(ctx2["shape"] ==
              rllm::to_hex(rllm::shape_hash(R2.as_object().at("system"),
                                            R2.as_object().at("tools"))),
          "shape = system+tools 的哈希");

    /* 反拼（服务端视角）：增量体 + 已知 blobs + 随体 marks + 按 shape 注入的
     * system/tools → 完整请求体 */
    boost::json::value W = boost::json::parse(wire2);
    const boost::json::object cx = W.as_object().at("revlm_context").as_object();
    W.as_object().erase("revlm_context");
    boost::json::value sys = R2.as_object().at("system");
    boost::json::value tls = R2.as_object().at("tools");
    rllm::strip_cache_control(sys); // shape 存的是剥过 cache_control 的那份
    rllm::strip_cache_control(tls);
    W.as_object()["system"] = std::move(sys);
    W.as_object()["tools"] = std::move(tls);
    std::vector<std::string> known{blobs2[0],
                                   rllm::canonical_message(protocol_assistant(kAssistant1))};
    for (auto &m : W.as_object().at("messages").as_array())
    {
        rllm::strip_cache_control(m); // 服务端收到增量体也会再剥一遍
        known.push_back(rllm::canonical_message(m));
    }
    const std::string reassembled =
        rllm::assemble_request_body(W, known, marks_from_json(cx.at("cache_control")));
    CHECK(reassembled == full2,
          "从增量体 + 已知 blobs 反拼 == 客户端本地完整体（逐字节，含键序）");
}

/* —— 单元：cache_control 的 marks 全量随体、路径按完整请求计 —— */
static void test_marks_roundtrip()
{
    printf("[cache_control marks]\n");
    const Config cfg = make_config();
    ContextSession cs;

    // 手工请求体：system 数组带断点、消息块里也带断点（upstream 今天不发
    // cache_control，这里直接造，验协议本身的搬运）
    json short_body = json::parse(R"({
      "model": "m-test",
      "max_tokens": 100,
      "stream": true,
      "system": [
        {"type":"text","text":"sys 1","cache_control":{"type":"ephemeral"}},
        {"type":"text","text":"sys 2"}
      ],
      "messages": [
        {"role":"user","content":[
          {"type":"text","text":"a"},
          {"type":"text","text":"b","cache_control":{"type":"ephemeral"}}]},
        {"role":"assistant","content":[{"type":"text","text":"ok"}]}
      ]})");
    cs.wrap_request(short_body.dump());

    json long_body = json::parse(short_body.dump());
    long_body["messages"].push_back(user_msg("next"));
    const std::string wire = cs.wrap_request(long_body.dump());
    const json w = json::parse(wire);
    CHECK(w["messages"].size() == 1, "增量只带尾部（marks 不影响消息条数）");
    const json &cc = w["revlm_context"]["cache_control"];
    CHECK(cc.is_array() && cc.size() == 2, "marks 全量随体（system 一处 + 消息块一处）");
    // strip 按文档序产出（请求键按字母序，messages 在 system 前），按 path 首元素认
    json sys_mark, msg_mark;
    for (const auto &m : cc)
        if (!m["path"].empty() && m["path"][0] == "system")
            sys_mark = m;
        else
            msg_mark = m;
    CHECK(sys_mark["path"] == json::array({"system", 0}) &&
              sys_mark["value"]["type"] == "ephemeral",
          "system 上的 mark 路径按完整请求计");
    CHECK(msg_mark["path"] == json::array({"messages", 0, "content", 1}),
          "消息块上的 mark 路径按完整请求的条数计");

    /* 反拼：断点插回去的位置与本地完整拼装一致。已知 blobs 取前两条消息（剥掉
     * cache_control 再 canonical，链条哈希的不参与） */
    boost::json::value W = boost::json::parse(wire);
    const boost::json::object cx = W.as_object().at("revlm_context").as_object();
    W.as_object().erase("revlm_context");
    boost::json::value head = boost::json::parse(short_body.dump());
    rllm::strip_cache_control(head);
    W.as_object()["system"] = head.as_object().at("system"); // 服务端按 shape 注入（剥过的）
    std::vector<std::string> known;
    for (const auto &m : head.as_object().at("messages").as_array())
        known.push_back(rllm::canonical_message(m));
    for (const auto &m : W.as_object().at("messages").as_array())
        known.push_back(rllm::canonical_message(m));
    const std::string reassembled =
        rllm::assemble_request_body(W, known, marks_from_json(cx.at("cache_control")));
    CHECK(reassembled == assemble_full(long_body.dump(), nullptr, nullptr, true),
          "断点插回后的反拼 == 完整拼装（cache_control 位置也一致，P3 键序）");
}

/* —— 单元：历史对不上 / shape 变了 → 完整请求 —— */
static void test_fallbacks()
{
    printf("[回退完整请求]\n");
    const Config cfg = make_config();
    ContextSession cs;

    const std::string r1 = build_request(cfg, dialog1("You are a coding agent.", true)).body;
    cs.wrap_request(r1);
    cs.record_assistant(kAssistant1);

    { // 回退：历史比已知前缀短（上下文压缩/rewind 后的形状）
        const std::string wire = cs.wrap_request(r1);
        const json w = json::parse(wire);
        CHECK(w["revlm_context"].is_object() && w["revlm_context"].empty(),
              "历史对不上已知前缀 → 退回 revlm_context:{}");
        CHECK(w["messages"].size() == 1, "完整体带全部消息");
    }
    { // shape 变了：system 换了内容，消息链没变也不能增量（省一次 409 往返）
        const std::string r2 =
            build_request(cfg, dialog3("Another system prompt.", true)).body;
        const std::string wire = cs.wrap_request(r2);
        const json w = json::parse(wire);
        CHECK(w["revlm_context"].is_object() && w["revlm_context"].empty(),
              "shape 变化 → 本地直接退回完整请求");
    }
    CHECK(cs.stats().incrementals == 0, "两次回退都不产生增量计数");
}

/* —— 单元：409 判别 —— */
static void test_conflict_detection()
{
    printf("[409 判别]\n");
    CHECK(ContextSession::is_conflict(
              409, R"({"type":"error","error":{"type":"revlm_context_miss","message":"prefix"}})"),
          "revlm_context_miss → 重发完整请求");
    CHECK(ContextSession::is_conflict(
              409,
              R"({"type":"error","error":{"type":"revlm_context_mismatch","message":"expect differs"}})"),
          "revlm_context_mismatch → 重发完整请求");
    CHECK(!ContextSession::is_conflict(
              409, R"({"type":"error","error":{"type":"overloaded_error","message":"busy"}})"),
          "其余 409 是真错误，不重发");
    CHECK(!ContextSession::is_conflict(
              400, R"({"type":"error","error":{"type":"revlm_context_miss","message":"prefix"}})"),
          "状态码不是 409 不算");
    CHECK(!ContextSession::is_conflict(409, "not json"), "错误体不是 JSON 不算");
}

/* —— 单元：统计 —— */
static void test_stats()
{
    printf("[统计]\n");
    const Config cfg = make_config();
    ContextSession cs;

    const std::string r1 = build_request(cfg, dialog1("You are a coding agent.", true)).body;
    cs.wrap_request(r1);
    cs.on_response(""); // 完整体成功：不算命中
    CHECK(cs.stats().hits == 0 && cs.stats().incrementals == 0, "首轮完整体不产生命中");

    cs.record_assistant(kAssistant1);
    const std::string r2 = build_request(cfg, dialog3("You are a coding agent.", true)).body;
    const std::string wire2 = cs.wrap_request(r2);
    CHECK(cs.stats().incrementals == 1, "第二轮发出增量体");
    cs.on_response("prefix=" + std::string(64, '0') + "; count=2; shape=" + std::string(64, '0'));
    CHECK(cs.stats().hits == 1, "增量成功 → 命中（服务端 prefix 不一致不影响落账）");

    // 节省字节 = 完整发送体 - 增量发送体；完整发送体独立构造（同一请求 + revlm_context:{}）
    boost::json::value R2f = boost::json::parse(r2);
    R2f.as_object()["revlm_context"] = boost::json::object{};
    const long long want =
        (long long)boost::json::serialize(R2f).size() - (long long)wire2.size();
    CHECK(cs.stats().bytes_saved == want, "节省字节 = 完整体大小 - 实际发送体大小");

    // 409：miss 计一次，重发后的成功不落命中
    const json second_reply{{"role", "assistant"},
                            {"content", json::array({json{{"type", "text"}, {"text", "second"}}})}};
    cs.record_assistant(second_reply);
    json d3 = dialog3("You are a coding agent.", true);
    d3["messages"].push_back(second_reply);
    d3["messages"].push_back(user_msg("third"));
    cs.wrap_request(build_request(cfg, d3).body);
    CHECK(cs.stats().incrementals == 2, "第三轮又是增量体");
    cs.resend_full();
    cs.on_response("prefix=" + std::string(64, '0') + "; count=4; shape=" + std::string(64, '0'));
    CHECK(cs.stats().misses == 1 && cs.stats().hits == 1,
          "409 记一次未命中；重发完整体成功不落命中");
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

/* 假 revlm：收下每个请求体；对第一个增量请求回一次 409，其余 200 SSE。
 * 200 的响应头故意带一个全零 prefix —— 客户端必须以本地为准。 */
class FakeRevlm {
  public:
    explicit FakeRevlm(std::string conflict_type) : type_(std::move(conflict_type))
    {
        svr_.Post("/v1/messages", [this](const httplib::Request &rq, httplib::Response &rs) {
            std::lock_guard<std::mutex> lk(mtx_);
            bodies_.push_back(rq.body);
            const json body = json::parse(rq.body, nullptr, false);
            const bool incremental = body.is_object() && body.contains("revlm_context") &&
                                     body["revlm_context"].is_object() &&
                                     body["revlm_context"].contains("prefix");
            if (incremental && conflicts_sent_ == 0)
            {
                ++conflicts_sent_;
                rs.status = 409;
                rs.set_content(
                    R"({"type":"error","error":{"type":")" + type_ + R"(","message":"prefix"}})",
                    "application/json");
                return;
            }
            rs.status = 200;
            rs.set_header("Content-Type", "text/event-stream");
            rs.set_header("X-Revlm-Context",
                          "prefix=" + std::string(64, '0') + "; count=1; shape=" +
                              std::string(64, '0'));
            rs.body = bodies_.size() == 1 ? sse_tool_use() : sse_text("SECOND_REPLY");
        });
        port_ = svr_.bind_to_any_port("127.0.0.1");
        th_ = std::thread([this] { svr_.listen_after_bind(); });
    }
    ~FakeRevlm()
    {
        svr_.stop();
        if (th_.joinable()) th_.join();
    }

    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::vector<std::string> bodies() const
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return bodies_;
    }

  private:
    std::string type_;
    httplib::Server svr_;
    int port_ = 0;
    std::thread th_;
    mutable std::mutex mtx_;
    std::vector<std::string> bodies_;
    int conflicts_sent_ = 0;
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

/* 409 两条路（miss / mismatch）走同一条剧本。同一个 Agent 跑两趟：会话状态跟着
 * Agent 走，新 Agent 是新会话（首轮必发完整请求）。 */
static void run_conflict_scenario(const char *type)
{
    FakeRevlm ep(type);
    const fs::path home =
        fs::temp_directory_path() / ("realagent-ctx-e2e-" + std::string(type));
    fs::remove_all(home);
    fs::create_directories(home / ".realagent");
    ::setenv("HOME", home.c_str(), 1);
    {
        json s{{"protocol", "anthropic-messages"},
               {"base_url", ep.base_url()},
               {"api_key", "k"},
               {"model", "m-main"},
               {"permission", "allow-all"},
               {"revlm_context", true}};
        std::ofstream(home / ".realagent" / "settings.json") << s.dump();
    }
    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: 配置加载失败\n");
        ++failures;
        return;
    }

    Frames f;
    CoreContext ctx{.config = &*cfg,
                    .pricing = nullptr,
                    .emit_fn = [&f](const std::string &t, const std::string &) { f.add(t); }};
    ApprovalCoordinator approval;
    {
        Agent a(ctx, approval, home.string(), 1);
        a.post("干活");
        CHECK(wait_end(f, 1), "第一趟收工（409 重发后恢复）");

        const std::vector<std::string> bodies1 = ep.bodies();
        CHECK(bodies1.size() == 3, "三次请求：完整 → 增量(409) → 完整重发");
        if (bodies1.size() == 3)
        {
            const json b0 = json::parse(bodies1[0]);
            CHECK(b0["revlm_context"].is_object() && b0["revlm_context"].empty() &&
                      b0["messages"].size() == 1,
                  "第 1 个请求：revlm_context:{} 带全部消息");
            const json b1 = json::parse(bodies1[1]);
            CHECK(b1["revlm_context"].contains("prefix") &&
                      b1["revlm_context"]["count"] == 2 && b1["messages"].size() == 1,
                  "第 2 个请求：增量（只带 tool_result 尾部）");
            const json b2 = json::parse(bodies1[2]);
            CHECK(b2["revlm_context"].is_object() && b2["revlm_context"].empty() &&
                      b2["messages"].size() == 3,
                  "409 后自动重发的是完整体");
        }
        const ContextSession::Stats st1 = a.context_session().stats();
        CHECK(st1.incrementals == 1 && st1.misses == 1 && st1.hits == 0,
              "统计：一次增量、一次 409 未命中、命中 0");

        /* 第二趟（同一 Agent）：无 409，增量命中；响应头的错 prefix 不覆盖本地 */
        a.post("再来");
        CHECK(wait_end(f, 2), "第二趟收工");

        const std::vector<std::string> bodies = ep.bodies();
        CHECK(bodies.size() == 4, "第二趟只多一个请求（增量命中，没再重发）");
        if (bodies.size() == 4)
        {
            const json b3 = json::parse(bodies[3]);
            CHECK(b3["revlm_context"].contains("prefix") && b3["messages"].size() == 1,
                  "第二趟仍是增量");

            /* prefix 本地复算：完整重发体（第 3 个请求）的三条消息 + assistant 回复 */
            std::vector<std::string> blobs;
            assemble_full(bodies[2], nullptr, &blobs);
            rllm::Node n = rllm::root_node();
            for (const auto &b : blobs)
                n = rllm::chain_node(n, rllm::blob_hash(boost::json::parse(b)));
            n = rllm::chain_node(
                n, rllm::blob_hash(protocol_assistant(
                       json{{"role", "assistant"},
                            {"content", json::array({json{{"type", "text"}, {"text", "SECOND_REPLY"}}})}})));
            CHECK(b3["revlm_context"]["prefix"] == rllm::to_hex(n) &&
                      b3["revlm_context"]["count"] == 4,
                  "prefix 仍是本地算的 node(4)——服务端回的全零 prefix 没有覆盖本地");
        }
        const ContextSession::Stats st = a.context_session().stats();
        CHECK(st.incrementals == 2 && st.misses == 1 && st.hits == 1,
              "统计：两次增量、一次未命中、一次命中");
    }
    fs::remove_all(home);
}

static void test_e2e_conflict_resend()
{
    printf("[端到端 409 重发：revlm_context_miss]\n");
    run_conflict_scenario("revlm_context_miss");
    printf("[端到端 409 重发：revlm_context_mismatch]\n");
    run_conflict_scenario("revlm_context_mismatch");
}

int main()
{
    test_full_then_incremental();
    test_marks_roundtrip();
    test_fallbacks();
    test_conflict_detection();
    test_stats();
    test_e2e_conflict_resend();
    printf(failures ? "\n%d 项失败\n" : "\n全部通过\n", failures);
    return failures ? 1 : 0;
}
