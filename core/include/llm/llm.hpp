/*
 * llm.hpp — 一次 LLM 调用的三件事：造请求、解响应、算钱（ADR-0017）
 *
 *   对话 ──build_request──▶ HttpRequest ─[curl]─▶ SSE ──feed_block──▶ 事件 ──▶ Pricing::cost
 *        upstream/<协议>.cpp                         downstream/<协议>.cpp
 *
 * 一个协议在一个方向上的全部知识（认证头、路径、体形状、帧结构、token 字段名）住在
 * 一个文件里：拆成独立开关就能配出无效组合。协议由用户选，没有默认、不从 URL 猜。
 *
 * 事件词汇只有一套：thinking_start / thinking_update / thinking_stop / message_update /
 * tool_use / usage / stop。usage 的字段统一叫 input / output / cache_read / cache_write。
 */
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "config.hpp"
#include "json.hpp"

namespace realagent {

/* 协议标签：重载的判别参数 */
namespace protocol {
struct AnthropicMessages {};
struct OpenAiChat {};
struct OpenAiResponses {};
} // namespace protocol

enum class Protocol { AnthropicMessages,
                      OpenAiChat,
                      OpenAiResponses };

/* 配置字符串 → 协议。认不出返回 nullopt。 */
std::optional<Protocol> protocol_from(std::string_view name);

/* protocol / base_url / model 配齐了返回空串；否则返回一段人话，附一段可照抄的配置。 */
std::string endpoint_config_error(const Config &cfg);

/* 非 2xx 时的人话错误（能从 body 里捞出 message 就捞）。status 0 = 没拿到响应，返回空串。 */
std::string http_status_error(long status, const std::string &body);

struct HttpRequest {
    std::string url;
    std::vector<std::string> headers; // "Key: value"
    std::string body;
};

/* —— 上行：{model, system, messages, tools} → 具体协议的请求 —— */
HttpRequest build_request(protocol::AnthropicMessages, const Config &cfg, const nlohmann::json &dialog);
HttpRequest build_request(protocol::OpenAiChat, const Config &cfg, const nlohmann::json &dialog);
HttpRequest build_request(protocol::OpenAiResponses, const Config &cfg, const nlohmann::json &dialog);
/* 按配置里的协议派发（调用前已由 endpoint_config_error 把关）。 */
HttpRequest build_request(const Config &cfg, const nlohmann::json &dialog);

/* payload 只在回调内有效 */
using EventSink = std::function<void(std::string_view type, const nlohmann::json &payload)>;

/* —— 下行：每个协议一份解析状态，一次调用一份 —— */

struct UsageCounts {
    long long input = 0;
    long long output = 0;
    long long cache_read = 0;
    long long cache_write = 0;
};

struct AnthropicState {
    using tag = protocol::AnthropicMessages;
    std::string block_type_; // text / thinking / tool_use
    std::string tool_id_;
    std::string tool_name_;
    std::string tool_input_; // 累积 partial_json
    std::string thinking_sig_;
    UsageCounts usage;
};

struct OpenAiChatState {
    using tag = protocol::OpenAiChat;
    struct PendingTool {
        std::string id;
        std::string name;
        std::string args;
    };
    std::unordered_map<long long, PendingTool> tools; // tool_calls 按下标增量到达
    std::vector<long long> tool_order;                // 首次出现的顺序
    bool reasoning_open = false;
    std::string finish_reason;
    UsageCounts usage;
};

struct OpenAiResponsesState {
    using tag = protocol::OpenAiResponses;
    std::string tool_id_;
    std::string tool_name_;
    std::string tool_input_;
    bool reasoning_open = false;
    std::string finish_reason;
    UsageCounts usage;
};

/* 一个 SSE 块里的 event: 行与 data: 行（多行 data 用 \n 相接）。 */
struct SseBlock {
    std::string event;
    std::string data;
};
SseBlock split_sse_block(std::string_view block);

/* 喂一个已切好的 SSE 块。畸形帧返回 false（本次调用应中止），绝不静默跳过：
 * 跳过 = 正文悄悄消失，上层收到"成功但空"。 */
bool feed_block(protocol::AnthropicMessages, AnthropicState &, std::string_view block,
                const EventSink &);
bool feed_block(protocol::OpenAiChat, OpenAiChatState &, std::string_view block, const EventSink &);
bool feed_block(protocol::OpenAiResponses, OpenAiResponsesState &, std::string_view block,
                const EventSink &);

/* 按空行切块（三套协议共有），切完交给协议自己的 feed_block。一次调用一个实例。 */
class SseParser {
  public:
    explicit SseParser(Protocol p);
    /* false = 帧不合规，本次调用应中止 */
    bool feed(std::string_view chunk, const EventSink &sink);

  private:
    std::string buf_;
    std::variant<AnthropicState, OpenAiChatState, OpenAiResponsesState> st_;
};

/* —— 下行文件共用 —— */

/* 全零不发。 */
void emit_usage(const UsageCounts &u, const EventSink &sink);

/* 各家给的字段并不齐全：缺失或类型不符按零值。 */
inline long long json_int(const nlohmann::json &o, const char *key)
{
    const auto it = o.find(key);
    return it != o.end() && it->is_number_integer() ? it->get<long long>() : 0;
}
inline std::string json_str(const nlohmann::json &o, const char *key)
{
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

/* 没有「思考块结束」帧的协议，在正文开始或收工时补一个 thinking_stop。 */
inline void close_thinking(bool &open, const EventSink &sink)
{
    if (!open) return;
    open = false;
    if (sink) sink("thinking_stop", nlohmann::json::object());
}

/* 模型表（ADR-0009）：单价 + 公开元数据。~/.realagent/models.json 存在就整表用它，
 * 否则用编译进来的出厂表，不合并。 */
class Pricing {
  public:
    /* 文件读不动或条目缺字段 → error 非空，返回空表。 */
    static Pricing load(const Config &cfg, std::string *error = nullptr);

    /* 同名键点积 / 1M。算不出返回 0。 */
    double cost(const std::string &model, const nlohmann::json &usage) const;

    /* [{name, owned_by, context}]，不含单价 */
    const nlohmann::json &models() const { return models_; }

  private:
    std::unordered_map<std::string, nlohmann::json> pricing_;
    nlohmann::json models_ = nlohmann::json::array();
};

} // namespace realagent
