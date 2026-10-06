/*
 * upstream/anthropic_messages.cpp — 抽象对话 → /v1/messages 请求
 *
 * thinking 块带 signature 原样回传。认证头两个一起发：原厂认 x-api-key，
 * 兼容端点认 Authorization: Bearer，原厂对多出来的那个视而不见，于是不必认对面是谁。
 */
#include "llm/llm.hpp"
#include "tools/tools.hpp"

namespace realagent {

namespace {

/* 工具结果块（MCP 形状）→ /v1/messages 块。图片改成 `source: {type: base64, ...}`；
 * 带不动的（音频、二进制资源）压成一行文字占位，不悄悄丢掉。 */
nlohmann::json to_tool_content(const nlohmann::json &content)
{
    nlohmann::json out = nlohmann::json::array();
    for (const nlohmann::json &b : content)
    {
        const std::string type = b.value("type", std::string());
        if (type == "text")
        {
            out.push_back({{"type", "text"}, {"text", b.value("text", std::string())}});
        }
        // 图片要 data 与 mimeType 齐全，缺一样就走占位，不替它猜
        else if (type == "image" && b.contains("data") && b.contains("mimeType"))
        {
            out.push_back({{"type", "image"},
                           {"source",
                            {{"type", "base64"},
                             {"media_type", b.at("mimeType")},
                             {"data", b.at("data")}}}});
        }
        else
        {
            // 其余（音频、resource_link、二进制资源）没有对应的块类型，走共用的那行占位
            out.push_back({{"type", "text"},
                           {"text", tool_content_text(nlohmann::json::array({b}))}});
        }
    }
    if (out.empty()) // 空 content 端点不收，给一句实话
        out.push_back({{"type", "text"}, {"text", "(no output)"}});
    return out;
}

} // namespace

HttpRequest build_request(protocol::AnthropicMessages, const Config &cfg, const nlohmann::json &dialog)
{
    const nlohmann::json &d = dialog;

    nlohmann::json body;
    body["model"] = d.value("model", std::string());
    body["max_tokens"] = 4096;
    body["stream"] = true;
    if (const std::string system = d.value("system", std::string()); !system.empty())
        body["system"] = system;

    // messages：抽象对话 → /v1/messages 格式（合并相邻同 role）
    nlohmann::json msgs = nlohmann::json::array();
    if (d.contains("messages") && d["messages"].is_array())
    {
        for (const nlohmann::json &m : d["messages"])
        {
            const std::string role = m.at("role");
            nlohmann::json blocks = nlohmann::json::array();
            if (m.contains("content") && m["content"].is_array())
            {
                for (const nlohmann::json &b : m["content"])
                {
                    const std::string bt = b.at("type");
                    nlohmann::json out_block = nlohmann::json::object();
                    if (bt == "text")
                    {
                        out_block["type"] = "text";
                        out_block["text"] = b.at("text");
                    }
                    else if (bt == "tool_use")
                    {
                        out_block["type"] = "tool_use";
                        out_block["id"] = b.at("id");
                        out_block["name"] = b.at("name");
                        out_block["input"] = b.value("input", nlohmann::json::object());
                    }
                    else if (bt == "tool_result")
                    {
                        out_block["type"] = "tool_result";
                        out_block["tool_use_id"] = b.at("tool_use_id");
                        out_block["content"] = to_tool_content(b.at("content"));
                        if (b.value("is_error", false)) out_block["is_error"] = true;
                    }
                    else if (bt == "thinking")
                    {
                        // thinking 块（协议固有内容）原样回传，带 signature（缺失时省略）
                        out_block["type"] = "thinking";
                        out_block["thinking"] = b.at("thinking");
                        if (b.contains("signature")) out_block["signature"] = b.at("signature");
                    }
                    blocks.push_back(out_block);
                }
            }
            // 合并相邻同 role：若上一条 message 同 role，并入其 content
            if (!msgs.empty() && msgs.back()["role"] == role)
            {
                nlohmann::json &last_blocks = msgs.back()["content"];
                for (nlohmann::json &blk : blocks) last_blocks.push_back(blk);
            }
            else
            {
                nlohmann::json mout;
                mout["role"] = role;
                mout["content"] = blocks;
                msgs.push_back(mout);
            }
        }
    }
    body["messages"] = msgs;

    if (d.contains("tools") && d["tools"].is_array())
    {
        nlohmann::json tools = nlohmann::json::array();
        for (const nlohmann::json &t : d["tools"])
        {
            nlohmann::json tool;
            tool["name"] = t.at("name");
            if (t.contains("description")) tool["description"] = t.at("description");
            tool["input_schema"] = t.value("input_schema", nlohmann::json::object());
            tools.push_back(tool);
        }
        body["tools"] = tools;
        body["tool_choice"] = nlohmann::json{{"type", "auto"}};
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/v1/messages";
    if (const std::string key = cfg.get("api_key"); !key.empty())
    {
        req.headers.push_back("x-api-key: " + key);
        req.headers.push_back("Authorization: Bearer " + key);
    }
    req.headers.push_back("Content-Type: application/json");
    req.headers.push_back("anthropic-version: 2023-06-01");
    req.body = body.dump();
    return req;
}

} // namespace realagent
