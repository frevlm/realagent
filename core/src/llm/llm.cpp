/*
 * llm.cpp — 三套协议共有的部分：协议名、端点校验、派发、SSE 切块、计价
 */
#include "llm/llm.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace realagent {
namespace fs = std::filesystem;

/* ==================== 协议身份 ==================== */

std::optional<Protocol> protocol_from(std::string_view name)
{
    if (name == "anthropic-messages") return Protocol::AnthropicMessages;
    if (name == "openai-chat") return Protocol::OpenAiChat;
    if (name == "openai-responses") return Protocol::OpenAiResponses;
    return std::nullopt;
}

/* ==================== 端点配置校验（ADR-0017）==================== */

std::string endpoint_config_error(const Config &cfg)
{
    std::vector<std::string> missing;
    if (cfg.get("protocol").empty()) missing.push_back("protocol");
    if (cfg.get("base_url").empty()) missing.push_back("base_url");
    if (cfg.get("model").empty()) missing.push_back("model");

    // 写错与没写分开说
    std::string bad_protocol;
    if (missing.empty() && !protocol_from(cfg.get("protocol")))
        bad_protocol = cfg.get("protocol");

    if (missing.empty() && bad_protocol.empty()) return {};

    std::string out;
    if (!missing.empty())
    {
        out = "配置缺少必填键：";
        for (std::size_t i = 0; i < missing.size(); ++i)
        {
            if (i) out += "、";
            out += missing[i];
        }
        out += "。这三个键没有默认值——填错产生的报错最难诊断，所以宁可现在拦住你。";
    }
    else
    {
        out = "配置里的 protocol=\"" + bad_protocol + "\" 不认识。";
    }
    out += "\nprotocol 只有三个值：anthropic-messages / openai-chat / openai-responses。";
    out += "\n往 ~/.realagent/settings.json 里写（照抄改值即可）：\n";
    out += R"({
  "protocol": "anthropic-messages",
  "base_url": "https://api.deepseek.com/anthropic",
  "model": "deepseek-v4-flash",
  "api_key": "sk-你的密钥"
})";
    return out;
}

std::string http_status_error(long status, const std::string &body)
{
    // 0 = 没拿到响应，交给 CURLcode 解释
    if (status == 0) return {};
    if (status >= 200 && status < 300) return {};
    std::string msg;
    // 各家错误体形状不同：捞得到 message 就用，捞不到给原文
    if (const nlohmann::json j = nlohmann::json::parse(body, nullptr, false); j.is_object())
    {
        msg = j.value("/message"_json_pointer, std::string());
        if (msg.empty()) msg = j.value("/error/message"_json_pointer, std::string());
    }
    if (msg.empty()) msg = body.substr(0, 400);
    return "端点返回 HTTP " + std::to_string(status) + (msg.empty() ? "" : "：" + msg);
}

/* ==================== 上行派发 ==================== */

HttpRequest build_request(const Config &cfg, const nlohmann::json &dialog)
{
    // 调用方已用 endpoint_config_error 把关；解不出来就是 bug，value() 抛出来
    const auto p = protocol_from(cfg.get("protocol"));
    switch (p.value())
    {
        case Protocol::AnthropicMessages:
            return build_request(protocol::AnthropicMessages{}, cfg, dialog);
        case Protocol::OpenAiChat:
            return build_request(protocol::OpenAiChat{}, cfg, dialog);
        case Protocol::OpenAiResponses:
            return build_request(protocol::OpenAiResponses{}, cfg, dialog);
    }
    return {};
}

/* ==================== 下行：切块 + 派发 ==================== */

SseBlock split_sse_block(std::string_view block)
{
    SseBlock out;
    for (size_t i = 0; i < block.size();)
    {
        const size_t nl = block.find('\n', i);
        std::string_view line =
            nl == std::string_view::npos ? block.substr(i) : block.substr(i, nl - i);
        i = nl == std::string_view::npos ? block.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);

        const auto after = [&line](size_t n) {
            std::string_view v = line.substr(n);
            if (!v.empty() && v.front() == ' ') v.remove_prefix(1);
            return v;
        };
        if (line.starts_with("event:"))
        {
            out.event = after(6);
        }
        else if (line.starts_with("data:"))
        {
            // 多行 data 按 SSE 规范用 \n 相接（openai-responses 的大载荷会分行）
            if (!out.data.empty()) out.data += '\n';
            out.data += after(5);
        }
    }
    return out;
}

SseParser::SseParser(Protocol p)
{
    switch (p)
    {
        case Protocol::AnthropicMessages:
            st_ = AnthropicState{};
            break;
        case Protocol::OpenAiChat:
            st_ = OpenAiChatState{};
            break;
        case Protocol::OpenAiResponses:
            st_ = OpenAiResponsesState{};
            break;
    }
}

bool SseParser::feed(std::string_view chunk, const EventSink &sink)
{
    buf_.append(chunk);
    // 按空行切事件块。\n\n 与 \r\n\r\n 都可能出现，取先到的那个边界。
    for (;;)
    {
        const size_t lf = buf_.find("\n\n");
        const size_t crlf = buf_.find("\r\n\r\n");
        size_t pos, sep_len;
        if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf))
        {
            pos = crlf;
            sep_len = 4;
        }
        else if (lf != std::string::npos)
        {
            pos = lf;
            sep_len = 2;
        }
        else
        {
            break;
        }
        const std::string block = buf_.substr(0, pos);
        buf_.erase(0, pos + sep_len);

        // 每个 State 自带 tag，状态的类型就是协议身份
        const bool ok = std::visit(
            [&](auto &s) {
                using State = std::decay_t<decltype(s)>;
                return feed_block(typename State::tag{}, s, block, sink);
            },
            st_);
        if (!ok) return false;
    }
    return true;
}

/* 各协议换算完的 usage 都从这儿出门。全零不发（无 usage 信息的端点保持静默） */
void emit_usage(const UsageCounts &u, const EventSink &sink)
{
    if (!sink) return;
    if (u.input == 0 && u.output == 0 && u.cache_read == 0 && u.cache_write == 0) return;
    sink("usage", nlohmann::json{{"input", u.input},
                                 {"output", u.output},
                                 {"cache_read", u.cache_read},
                                 {"cache_write", u.cache_write}});
}

/* ==================== 模型数据表 / 计价 ==================== */

/* 出厂模型表（ADR-0009），编译进二进制。~/.realagent/models.json 存在即整表替换。 */
static constexpr const char *kFactoryModels = R"([
  {"name":"deepseek-v4-flash","owned_by":"deepseek","context":1048576,
   "pricing":{"input":0.14,"output":0.28,"cache_read":0.0028,"cache_write":0}},
  {"name":"deepseek-v4-pro","owned_by":"deepseek","context":1048576,
   "pricing":{"input":0.435,"output":0.87,"cache_read":0.003625,"cache_write":0}}
])";

Pricing Pricing::load(const Config &cfg, std::string *error)
{
    std::string text = kFactoryModels;
    const std::string path = cfg.models_path();
    if (std::error_code ec; fs::exists(path, ec))
    {
        std::ifstream f(path);
        if (!f)
        {
            if (error) *error = "模型数据表打不开: " + path;
            return {};
        }
        text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    if (!parsed.is_array())
    {
        if (error) *error = "模型数据表不是 JSON 数组: " + path;
        return {};
    }
    Pricing out;
    for (const nlohmann::json &m : parsed)
    {
        // 字段缺一即失败：半份表比没有表更难查
        const auto name = m.find("name");
        const auto owned_by = m.find("owned_by");
        const auto context = m.find("context");
        const auto pricing = m.find("pricing");
        if (name == m.end() || !name->is_string() || owned_by == m.end() ||
            !owned_by->is_string() || context == m.end() || !context->is_number_integer() ||
            pricing == m.end() || !pricing->is_object())
        {
            if (error)
                *error = "模型数据表条目缺字段（name/owned_by/context/pricing）: " + m.dump();
            return {};
        }
        out.pricing_[*name] = *pricing;
        out.models_.push_back(nlohmann::json{
            {"name", *name}, {"owned_by", *owned_by}, {"context", *context}});
    }
    return out;
}

double Pricing::cost(const std::string &model, const nlohmann::json &usage) const
{
    const auto it = pricing_.find(model);
    if (it == pricing_.end() || !usage.is_object()) return 0;
    double total = 0;
    for (const auto &[k, tokens] : usage.items())
    {
        const auto unit = it->second.find(k);
        if (unit == it->second.end() || !unit->is_number() || !tokens.is_number()) continue;
        total += tokens.get<double>() * unit->get<double>() / 1e6;
    }
    return total;
}

} // namespace realagent
