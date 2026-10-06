/*
 * test_llm.cpp — llm 模块单元测试（不碰网络）
 *
 *   build_request         : 抽象对话 → 三套协议各自的请求形状与认证头
 *   SseParser             : SSE → 事件（三套协议各自的帧结构，产出同一套事件词汇）
 *   endpoint_config_error : 端点那一束缺键/协议名写错 → 人话（ADR-0017）
 *   http_status_error     : 4xx/5xx → 人话，绝不当成"内容为空的成功"
 *   Pricing               : token 用量 → 钱；models() 不报单价
 *
 * 从前这里要 dlopen 两个容器、装一个假 host、手工按能力键取函数指针（ADR-0016 前）。
 * 现在就是调几个函数。
 */
#include "llm/llm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

/* 配置来自 ~/.realagent/settings.json（Config::load 的唯一覆盖来源）。
 * 测试把 HOME 指到临时目录，于是既能摆一份自己的 settings.json，
 * 也不会读到、写到用户真正的配置。 */
static Config make_config(const std::string &base_url, const std::string &api_key,
                          const std::string &models_json = "",
                          const std::string &proto = "anthropic-messages")
{
    static std::string home;
    home = (fs::temp_directory_path() / "realagent_test_home").string();
    fs::remove_all(home);
    fs::create_directories(home + "/.realagent");
    {
        std::ofstream f(home + "/.realagent/settings.json");
        f << R"({"protocol":")" << proto << R"(","base_url":")" << base_url
          << R"(","api_key":")" << api_key << R"(","model":"m-test"})";
    }
    if (!models_json.empty())
    {
        std::ofstream f(home + "/.realagent/models.json");
        f << models_json;
    }
    setenv("HOME", home.c_str(), 1);
    auto cfg = Config::load();
    if (!cfg)
    {
        printf("  FAIL: Config::load: %s\n", cfg.error().c_str());
        ++failures;
        std::exit(1);
    }
    return std::move(*cfg);
}

/* 事件收集器：一次调用喂进若干 chunk，把产出的事件按类型攒起来 */
struct Collected {
    std::vector<json> updates;
    std::vector<json> tools;
    std::vector<json> usages;
    std::vector<std::string> order;
    std::string stop_reason;
    std::string thinking;     // thinking_update 增量拼接
    std::string thinking_sig; // thinking_start 的 signature
    int thinking_stops = 0;
    bool ok = true;
};

static Collected run_parse(const std::vector<std::string> &chunks,
                           Protocol proto = Protocol::AnthropicMessages)
{
    Collected c;
    SseParser p(proto);
    const EventSink sink = [&c](std::string_view t, const json &ev) {
        c.order.emplace_back(t);
        if (t == "message_update")
            c.updates.push_back(ev);
        else if (t == "tool_use")
            c.tools.push_back(ev);
        else if (t == "usage")
            c.usages.push_back(ev);
        else if (t == "stop")
            c.stop_reason = ev["reason"];
        else if (t == "thinking_start")
            c.thinking_sig = ev["signature"];
        else if (t == "thinking_update")
            c.thinking += ev["delta"].get<std::string>();
        else if (t == "thinking_stop")
            ++c.thinking_stops;
    };
    for (const auto &ch : chunks)
        if (!p.feed(ch, sink)) c.ok = false;
    return c;
}

/* —— 工具结果的块数组：能带的带过去，带不动的压成一行占位（ADR-0023 §3） —— */
static void test_tool_result_blocks()
{
    printf("[tool_result 块数组]\n");
    const json dialog = json::parse(R"({
      "model": "m",
      "messages": [
        {"role": "assistant", "content": [{"type":"tool_use","id":"t1","name":"shot","input":{}}]},
        {"role": "user", "content": [{"type":"tool_result","tool_use_id":"t1","content":[
            {"type":"text","text":"here it is:"},
            {"type":"image","data":"aaaaaaaa","mimeType":"image/png"}]}]}
      ]})");

    { // anthropic-messages：图片过得去，但要换成它的 source 形状
        const Config cfg = make_config("http://x", "k", "", "anthropic-messages");
        const json body = json::parse(build_request(cfg, dialog).body);
        const json &c = body["messages"][1]["content"][0]["content"];
        CHECK(c.is_array() && c.size() == 2, "块数组原样是两块");
        CHECK(c[1]["type"] == "image" && c[1]["source"]["type"] == "base64" &&
                  c[1]["source"]["media_type"] == "image/png" &&
                  c[1]["source"]["data"] == "aaaaaaaa",
              "图片翻成 /v1/messages 的 source 形状（工具那边照 MCP 是 {data,mimeType}）");
    }
    { // openai-chat：工具消息只收字符串，非文本块压成占位
        const Config cfg = make_config("http://x/v1", "k", "", "openai-chat");
        const json body = json::parse(build_request(cfg, dialog).body);
        std::string tool_msg;
        for (const auto &m : body["messages"])
            if (m.value("role", std::string()) == "tool") tool_msg = m["content"];
        CHECK(tool_msg.find("here it is:") != std::string::npos, "文本过去了");
        CHECK(tool_msg.find("image/png") != std::string::npos &&
                  tool_msg.find("带不动") != std::string::npos,
              "图片压成一行占位——模型知道自己没看见，就不会假装看见了");
    }
    { // openai-responses：同一条规则，落在 function_call_output 的 output 里
        const Config cfg = make_config("http://x/v1", "k", "", "openai-responses");
        const json body = json::parse(build_request(cfg, dialog).body);
        std::string outp;
        for (const auto &i : body["input"])
            if (i.value("type", std::string()) == "function_call_output") outp = i["output"];
        CHECK(outp.find("here it is:") != std::string::npos &&
                  outp.find("image/png") != std::string::npos,
              "文本 + 占位，同一条规则");
    }
}

/* —— 1. build_request —— */
static void test_build_request()
{
    printf("[build_request]\n");
    const Config cfg = make_config("http://127.0.0.1:18080", "test-key-123");
    const json dialog = json::parse(R"({
        "model": "deepseek-v4-flash",
        "system": "You are a coding agent.",
        "messages": [
            {"role":"user","content":[{"type":"text","text":"hello"}]},
            {"role":"assistant","content":[
                {"type":"thinking","thinking":"先分析问题","signature":"sig-abc"},
                {"type":"text","text":"world"}
            ]}
        ],
        "tools": [
            {"name":"read","description":"读文件","input_schema":{"type":"object","properties":{"file_path":{"type":"string"}}}}
        ]
    })");
    const HttpRequest req = build_request(cfg, dialog);

    CHECK(req.url == "http://127.0.0.1:18080/v1/messages", "url = base_url + /v1/messages");
    std::string hdrs;
    for (const auto &h : req.headers) hdrs += h + "\n";
    CHECK(hdrs.find("x-api-key: test-key-123") != std::string::npos,
          "headers 含 x-api-key（Anthropic 原厂认这个）");
    CHECK(hdrs.find("Authorization: Bearer test-key-123") != std::string::npos,
          "headers 同时含 Bearer（DeepSeek 一类兼容端点认这个）——同一凭证两个名字");
    CHECK(hdrs.find("anthropic-version") != std::string::npos,
          "headers 含 anthropic-version（协议固有）");

    const json body = json::parse(req.body, nullptr, false);
    CHECK(!body.is_discarded(), "body 是合法 JSON");
    if (body.is_discarded()) return;
    CHECK(body["stream"] == true, "stream=true");
    CHECK(body["model"] == "deepseek-v4-flash", "model 从对话透传");
    const json asst = body["messages"][1];
    CHECK(asst["content"][0]["type"] == "thinking",
          "assistant[0].type=thinking");
    CHECK(asst["content"][0]["thinking"] == "先分析问题",
          "thinking 内容原样回传");
    CHECK(asst["content"][0]["signature"] == "sig-abc",
          "thinking signature 原样回传");
    CHECK(asst["content"][1]["type"] == "text",
          "assistant[1].type=text（thinking 块在正文前）");
    CHECK(body["tools"][0]["name"] == "read", "tools 带上工具清单");
}

/* —— openai-chat 的请求形状 —— */
static void test_build_request_openai_chat()
{
    printf("[build_request openai-chat]\n");
    const Config cfg = make_config("http://127.0.0.1:18080/v1", "k-oa", "", "openai-chat");
    const json dialog = json::parse(R"({
        "model": "gpt-x",
        "system": "You are a coding agent.",
        "messages": [
            {"role":"user","content":[{"type":"text","text":"hello"}]},
            {"role":"assistant","content":[{"type":"tool_use","id":"c1","name":"read","input":{"file_path":"a.txt"}}]},
            {"role":"user","content":[{"type":"tool_result","tool_use_id":"c1","content":[{"type":"text","text":"file body"}]}]}
        ],
        "tools": [{"name":"read","description":"读文件","input_schema":{"type":"object"}}]
    })");
    const HttpRequest req = build_request(cfg, dialog);
    CHECK(req.url == "http://127.0.0.1:18080/v1/chat/completions", "url = base_url + /chat/completions");
    std::string hdrs;
    for (const auto &h : req.headers) hdrs += h + "\n";
    CHECK(hdrs.find("Authorization: Bearer k-oa") != std::string::npos, "只发 Bearer");
    CHECK(hdrs.find("x-api-key") == std::string::npos, "不发 x-api-key（那是另一套协议的事）");

    const json body = json::parse(req.body, nullptr, false);
    CHECK(!body.is_discarded(), "body 是合法 JSON");
    if (body.is_discarded()) return;
    CHECK(body["messages"][0]["role"] == "system",
          "system 是 messages 里的一条，不是顶层字段");
    CHECK(body["stream_options"]["include_usage"] == true,
          "开 stream_options.include_usage，否则流里没有 usage，计价拿不到数");
    const json asst = body["messages"][2];
    CHECK(asst["tool_calls"][0]["function"]["name"] == "read",
          "tool_use → tool_calls[].function");
    CHECK(asst["tool_calls"][0]["function"]["arguments"].is_string(),
          "arguments 是 JSON 字符串，不是对象（本协议如此）");
    const json toolmsg = body["messages"][3];
    CHECK(toolmsg["role"] == "tool",
          "tool_result → 单独一条 role=tool 的 message");
    CHECK(toolmsg["tool_call_id"] == "c1", "靠 tool_call_id 关联");
    CHECK(body["tools"][0]["type"] == "function",
          "工具套一层 function");
}

/* —— openai-responses 的请求形状 —— */
static void test_build_request_openai_responses()
{
    printf("[build_request openai-responses]\n");
    const Config cfg = make_config("http://127.0.0.1:18080/v1", "k-rs", "", "openai-responses");
    const json dialog = json::parse(R"({
        "model": "gpt-x",
        "system": "sys text",
        "messages": [
            {"role":"user","content":[{"type":"text","text":"hello"}]},
            {"role":"assistant","content":[{"type":"tool_use","id":"c9","name":"read","input":{"p":1}}]},
            {"role":"user","content":[{"type":"tool_result","tool_use_id":"c9","content":[{"type":"text","text":"out"}]}]}
        ],
        "tools": [{"name":"read","input_schema":{"type":"object"}}]
    })");
    const HttpRequest req = build_request(cfg, dialog);
    CHECK(req.url == "http://127.0.0.1:18080/v1/responses", "url = base_url + /responses");
    const json body = json::parse(req.body, nullptr, false);
    CHECK(!body.is_discarded(), "body 是合法 JSON");
    if (body.is_discarded()) return;
    CHECK(body["instructions"] == "sys text",
          "system 走顶层 instructions");
    CHECK(body["input"][0]["content"][0]["type"] == "input_text",
          "用户侧文本叫 input_text");
    CHECK(body["input"][1]["type"] == "function_call",
          "tool_use → function_call item");
    CHECK(body["input"][1]["call_id"] == "c9", "关联键叫 call_id");
    CHECK(body["input"][2]["type"] == "function_call_output",
          "tool_result → function_call_output item");
    CHECK(body["tools"][0]["name"] == "read",
          "工具是平的，不套 function");
}

/* —— 端点配置校验（ADR-0017）：缺键、协议名写错 —— */
static void test_endpoint_config_error()
{
    printf("[endpoint_config_error]\n");
    {
        const Config cfg = make_config("http://x", "k");
        CHECK(endpoint_config_error(cfg).empty(), "三个键齐全 → 无错");
    }
    {
        // 只配 api_key：三个必填键一个都没有
        static std::string home = (fs::temp_directory_path() / "realagent_cfgerr_home").string();
        fs::remove_all(home);
        fs::create_directories(home + "/.realagent");
        {
            std::ofstream f(home + "/.realagent/settings.json");
            f << R"({"api_key":"k"})";
        }
        setenv("HOME", home.c_str(), 1);
        const auto cfg = Config::load();
        CHECK(cfg.has_value(), "缺必填键不影响 load 本身（load 只管 JSON 读不读得懂）");
        const std::string e = endpoint_config_error(*cfg);
        CHECK(e.find("protocol") != std::string::npos, "报错点名 protocol");
        CHECK(e.find("base_url") != std::string::npos, "报错点名 base_url");
        CHECK(e.find("model") != std::string::npos, "缺的三个一次报全，不是报一个改一个");
        CHECK(e.find("anthropic-messages") != std::string::npos &&
                  e.find("openai-chat") != std::string::npos &&
                  e.find("openai-responses") != std::string::npos,
              "把三个可选值都列出来");
        CHECK(e.find("settings.json") != std::string::npos, "附可直接抄的样例");
    }
    {
        const Config cfg = make_config("http://x", "k", "", "anthropic_messages"); // 下划线，写错了
        const std::string e = endpoint_config_error(cfg);
        CHECK(e.find("不认识") != std::string::npos,
              "协议名写错与没写分开说——他确实写了，只是拼错了");
    }
}

/* —— HTTP 状态码：4xx/5xx 绝不当成"内容为空的成功"（ADR-0017）—— */
static void test_http_status_error()
{
    printf("[http_status_error]\n");
    CHECK(http_status_error(0, "").empty(),
          "状态码 0 = 没拿到响应（连不上/被中断掐断）→ 那是 CURLcode 的事，"
          "在这儿说\"HTTP 0\"是拿一个不存在的状态码糊弄人");
    CHECK(http_status_error(200, "").empty(), "2xx → 无错");
    CHECK(http_status_error(299, "").empty(), "2xx 上界 → 无错");
    const std::string e401 = http_status_error(
        401, R"({"type":"error","error":{"type":"authentication_error","message":"API key is invalid."}})");
    CHECK(e401.find("401") != std::string::npos, "带上状态码");
    CHECK(e401.find("API key is invalid.") != std::string::npos,
          "把端点自己那句话捞出来给人看");
    const std::string eplain = http_status_error(500, "upstream exploded");
    CHECK(eplain.find("upstream exploded") != std::string::npos,
          "读不懂的错误体就把原文给他，绝不因为没读懂而说成成功");
}

/* —— 2. SSE 解析：正文 + tool_use + thinking —— */
static void test_parse_text()
{
    printf("[SseParser 正文]\n");
    // 分两段喂（模拟 curl chunk 边界切在事件中间）
    const auto c = run_parse({
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"你好\"}}\n\n",
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"世界\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n",
    });
    CHECK(c.ok, "解析全程无错");
    CHECK(c.updates.size() == 2, "产出 2 个 message_update");
    CHECK(c.updates.size() >= 2 && c.updates[0]["delta"] == "你好",
          "message_update[0] 文本=你好");
    CHECK(c.updates.size() >= 2 && c.updates[1]["delta"] == "世界",
          "message_update[1] 文本=世界（跨 chunk）");
    CHECK(c.stop_reason == "end_turn", "stop reason=end_turn");
}

static void test_parse_tool_use()
{
    printf("[SseParser tool_use]\n");
    const auto c = run_parse({
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"read\",\"input\":{}}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"file_path\\\":\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"a.txt\\\"}\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n",
    });
    CHECK(c.ok, "解析全程无错");
    CHECK(c.tools.size() == 1, "产出 1 个 tool_use");
    if (!c.tools.empty())
    {
        CHECK(c.tools[0]["name"] == "read", "tool_use.name=read");
        CHECK(c.tools[0]["input"]["file_path"] == "a.txt",
              "tool_use.input.file_path=a.txt（partial_json 累积）");
    }
    CHECK(c.stop_reason == "tool_use", "stop reason=tool_use");
}

static void test_parse_thinking()
{
    printf("[SseParser thinking]\n");
    const auto c = run_parse({
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,"
        "\"content_block\":{\"type\":\"thinking\",\"thinking\":\"初始想法\",\"signature\":\"sig-1\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"我需要\"}}\n\n",
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"先读文件\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,"
        "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"答案\"}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n",
    });
    CHECK(c.thinking_sig == "sig-1", "thinking_start 带 signature");
    CHECK(c.thinking == "初始想法我需要先读文件",
          "thinking_update 增量拼接（起始文本 + 跨 chunk delta）");
    CHECK(c.thinking_stops == 1, "thinking_stop 产出 1 次");
    CHECK(!c.updates.empty() && c.updates[0]["delta"] == "答案",
          "thinking 块结束后 text 块正常解析");
    CHECK(c.stop_reason == "end_turn", "stop reason=end_turn");
}

/* CRLF 分隔的 SSE（部分端点这么发）也得认 */
static void test_parse_crlf()
{
    printf("[SseParser CRLF 分隔]\n");
    const auto c = run_parse({
        "event: content_block_delta\r\ndata: {\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"crlf\"}}\r\n\r\n",
    });
    CHECK(c.updates.size() == 1 && c.updates[0]["delta"] == "crlf",
          "\\r\\n\\r\\n 分隔的事件块照常解析");
}

/* 畸形帧绝不静默跳过：报错让上层中止本次调用 */
static void test_parse_malformed()
{
    printf("[SseParser 畸形帧]\n");
    const auto c = run_parse({
        "event: content_block_start\ndata: {\"type\":\"content_block_start\"}\n\n", // 缺 content_block
    });
    CHECK(!c.ok, "畸形帧 → feed 返回 false（上层中止本次调用，不静默跳过）");
}

/* —— 3. usage：message_start 给 input，message_delta 给 output，合并上报 —— */
static void test_usage()
{
    printf("[SseParser usage]\n");
    const auto c = run_parse({
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"m1\","
        "\"usage\":{\"input_tokens\":1200,\"output_tokens\":1,"
        "\"cache_read_input_tokens\":300,\"cache_creation_input_tokens\":0}}}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":88},"
        "\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n",
    });
    CHECK(c.usages.size() == 2, "产出 2 个 usage 事件（首帧 + 终帧）");
    if (c.usages.size() == 2)
    {
        const json &last = c.usages.back();
        CHECK(last["input"] == 1200, "input 保留自 message_start");
        CHECK(last["output"] == 88, "output 取 message_delta 的终值");
        CHECK(last["cache_read"] == 300, "cache_read 合并上报");
    }
    // usage 必须先于 stop：下游收工时数字得已经定了
    const auto usage_at = std::find(c.order.begin(), c.order.end(), std::string("usage"));
    const auto stop_at = std::find(c.order.begin(), c.order.end(), std::string("stop"));
    CHECK(usage_at < stop_at, "usage 事件先于 stop 事件");
}

/* —— 4. 计价与模型清单 —— */
static void test_pricing()
{
    printf("[Pricing]\n");
    // 单价取整数好算：input 1000/1M，77 token → 0.077
    const Config cfg = make_config("http://x", "k", R"([
      {"name":"test-model","owned_by":"tester","context":131072,
       "pricing":{"input":1000,"output":2000}}
    ])");
    std::string err;
    const Pricing p = Pricing::load(cfg, &err);
    CHECK(err.empty(), "用户接管的模型数据表读入成功");

    const json usage = json::parse(R"({"input":77,"output":0,"cache_read":0,"cache_write":0})");
    const double cost = p.cost("test-model", usage);
    CHECK(cost > 0.0769 && cost < 0.0771, "cost = input 77 × 1000/1M = 0.077");
    CHECK(p.cost("test-model", json::parse(R"({"input":0,"output":0})")) == 0,
          "无用量则不产生费用");
    CHECK(p.cost("不在表里", usage) == 0, "表里没这个模型 → 不计价，不猜");

    const json &models = p.models();
    CHECK(models.size() == 1, "清单是 JSON 数组（1 条）");
    CHECK(models[0]["name"] == "test-model", "name 报上来");
    CHECK(models[0]["owned_by"] == "tester", "owned_by 报上来");
    CHECK(models[0]["context"] == 131072, "context 报上来");
    CHECK(!models[0].contains("pricing"), "单价不进公开清单");
}

/* 没有用户接管版时用出厂表——装完就能算钱，不必先摆一份表 */
static void test_pricing_factory()
{
    printf("[Pricing 出厂表]\n");
    const Config cfg = make_config("http://x", "k");
    std::string err;
    const Pricing p = Pricing::load(cfg, &err);
    CHECK(err.empty(), "出厂表读入成功");
    CHECK(p.models().size() > 0, "出厂表非空");
}

/* 坏表就是坏表：报错，不跳过坏条目、不补默认值 */
static void test_pricing_bad_table()
{
    printf("[Pricing 坏表]\n");
    const Config cfg = make_config("http://x", "k", R"([{"name":"m"}])"); // 缺 owned_by 等
    std::string err;
    const Pricing p = Pricing::load(cfg, &err);
    CHECK(!err.empty(), "条目缺字段 → 报错");
    CHECK(p.models().size() == 0, "报错时不留半份表");
}

/* —— openai-chat 的下行：终点是 [DONE]，工具按 index 攒 —— */
static void test_parse_openai_chat()
{
    printf("[解析 openai-chat]\n");
    const auto c = run_parse(
        {"data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"想一下\"}}]}\n\n",
         "data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}\n\n",
         "data: {\"choices\":[{\"delta\":{\"content\":\"世界\"}}]}\n\n",
         "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n",
         "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":8,"
         "\"prompt_tokens_details\":{\"cached_tokens\":100}}}\n\n",
         "data: [DONE]\n\n"},
        Protocol::OpenAiChat);
    CHECK(c.ok, "解析全程无错");
    CHECK(c.updates.size() == 2, "产出 2 个 message_update");
    CHECK(c.thinking == "想一下", "reasoning_content → thinking_update");
    CHECK(c.thinking_stops == 1, "正文开始即思考结束（本协议不另发结束帧）");
    CHECK(c.stop_reason == "stop", "finish_reason=stop");
    CHECK(c.usages.size() == 1, "产出 1 个 usage 事件");
    if (!c.usages.empty())
    {
        const json u = c.usages.back();
        CHECK(u["input"] == 120, "prompt_tokens → input");
        CHECK(u["output"] == 8, "completion_tokens → output");
        CHECK(u["cache_read"] == 100,
              "prompt_tokens_details.cached_tokens → cache_read");
    }
}

static void test_parse_openai_chat_tools()
{
    printf("[解析 openai-chat 工具调用]\n");
    const auto c = run_parse(
        {"data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
         "\"function\":{\"name\":\"read\",\"arguments\":\"\"}}]}}]}\n\n",
         "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"{\\\"file_\"}}]}}]}\n\n",
         "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
         "\"function\":{\"arguments\":\"path\\\":\\\"a.txt\\\"}\"}}]}}]}\n\n",
         "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"},
        Protocol::OpenAiChat);
    CHECK(c.ok, "解析全程无错");
    CHECK(c.tools.size() == 1, "分片累积成 1 个 tool_use");
    if (!c.tools.empty())
    {
        CHECK(c.tools[0]["id"] == "call_1", "id 取自首帧");
        CHECK(c.tools[0]["name"] == "read", "name 取自首帧");
        CHECK(c.tools[0]["input"]["file_path"] == "a.txt",
              "arguments 分片拼完再解析");
    }
    CHECK(c.stop_reason == "tool_use",
          "finish_reason=tool_calls 归一成 tool_use（上层只认一套词汇）");
    // 顺序要紧：上层收到 stop 就当本轮完了，之后来的 tool_use 没人接
    const auto it_tool = std::find(c.order.begin(), c.order.end(), "tool_use");
    const auto it_stop = std::find(c.order.begin(), c.order.end(), "stop");
    CHECK(it_tool < it_stop, "tool_use 先于 stop 发出");
}

/* 流内错误帧（HTTP 200 但载荷是错误）必须让本次调用失败 */
static void test_parse_openai_chat_error_frame()
{
    printf("[解析 openai-chat 流内错误]\n");
    const auto c = run_parse({"data: {\"error\":{\"message\":\"rate limited\"}}\n\n"},
                             Protocol::OpenAiChat);
    CHECK(!c.ok, "流里的 error 帧 → feed 返回 false，不静默当成没内容");
}

/* —— openai-responses 的下行：类型在 event: 行上 —— */
static void test_parse_openai_responses()
{
    printf("[解析 openai-responses]\n");
    const auto c = run_parse(
        {"event: response.reasoning_summary_text.delta\n"
         "data: {\"delta\":\"琢磨\"}\n\n",
         "event: response.output_text.delta\ndata: {\"delta\":\"答案\"}\n\n",
         "event: response.output_item.added\n"
         "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"fc_1\",\"name\":\"read\"}}\n\n",
         "event: response.function_call_arguments.delta\n"
         "data: {\"delta\":\"{\\\"file_path\\\":\\\"b.txt\\\"}\"}\n\n",
         "event: response.output_item.done\n"
         "data: {\"item\":{\"type\":\"function_call\",\"call_id\":\"fc_1\",\"name\":\"read\"}}\n\n",
         "event: response.completed\n"
         "data: {\"response\":{\"usage\":{\"input_tokens\":70,\"output_tokens\":5,"
         "\"input_tokens_details\":{\"cached_tokens\":20}}}}\n\n"},
        Protocol::OpenAiResponses);
    CHECK(c.ok, "解析全程无错");
    CHECK(c.thinking == "琢磨", "reasoning_summary_text.delta → thinking_update");
    CHECK(c.updates.size() == 1 && c.updates[0]["delta"] == "答案",
          "output_text.delta → message_update");
    CHECK(c.tools.size() == 1, "产出 1 个 tool_use");
    if (!c.tools.empty())
    {
        CHECK(c.tools[0]["id"] == "fc_1", "id 取 call_id");
        CHECK(c.tools[0]["input"]["file_path"] == "b.txt",
              "arguments 增量拼完再解析");
    }
    CHECK(c.stop_reason == "tool_use", "有工具调用 → 收工理由是 tool_use");
    if (!c.usages.empty())
    {
        const json u = c.usages.back();
        CHECK(u["input"] == 70, "input_tokens → input");
        CHECK(u["cache_read"] == 20,
              "input_tokens_details.cached_tokens → cache_read");
    }
}

static void test_parse_openai_responses_failed()
{
    printf("[解析 openai-responses 失败帧]\n");
    const auto c = run_parse({"event: response.failed\ndata: {\"response\":{\"error\":"
                              "{\"message\":\"boom\"}}}\n\n"},
                             Protocol::OpenAiResponses);
    CHECK(!c.ok, "response.failed → feed 返回 false（HTTP 是 200，但这次调用没成）");
}

int main()
{
    test_build_request();
    test_tool_result_blocks();
    test_build_request_openai_chat();
    test_build_request_openai_responses();
    test_endpoint_config_error();
    test_http_status_error();
    test_parse_text();
    test_parse_tool_use();
    test_parse_thinking();
    test_parse_crlf();
    test_parse_malformed();
    test_usage();
    test_parse_openai_chat();
    test_parse_openai_chat_tools();
    test_parse_openai_chat_error_frame();
    test_parse_openai_responses();
    test_parse_openai_responses_failed();
    test_pricing();
    test_pricing_factory();
    test_pricing_bad_table();
    printf(failures ? "\n%d 项失败\n" : "\n全部通过\n", failures);
    return failures ? 1 : 0;
}
