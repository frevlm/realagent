/*
 * upstream/openai_chat.cpp — 抽象对话 → /chat/completions 请求
 *
 * 与 anthropic_messages 的三处实质差异（不是风格差异，是协议差异）：
 *   1. system 是 messages 里的一条（role=system），不是顶层字段
 *   2. 一条 message 的 content 是字符串，不是块数组；工具调用另开 tool_calls 字段，
 *      工具结果另开一条 role=tool 的 message
 *   3. 工具参数是 JSON **字符串**，不是对象
 *
 * thinking 块不回传：本协议没有"把上一轮的思考原样交回去"这回事，
 * reasoning 内容是只读的产出。硬塞进 content 会被当成用户可见正文，那是另一种谎。
 */
#include "llm/llm.hpp"
#include "tools/tools.hpp"

namespace realagent {

namespace {

/* 块数组 → 一段文本（本协议的 content 只有字符串这一种形状） */
std::string join_text(const nlohmann::json &blocks)
{
    std::string out;
    for (const nlohmann::json &b : blocks)
    {
        if (b.at("type") != "text") continue;
        out += b.at("text").get<std::string>();
    }
    return out;
}

} // namespace

HttpRequest build_request(protocol::OpenAiChat, const Config &cfg, const nlohmann::json &dialog)
{
    const nlohmann::json &d = dialog;

    nlohmann::json body;
    body["model"] = d.value("model", std::string());
    body["max_tokens"] = 4096;
    body["stream"] = true;
    // 不问就不给：本协议默认的流不带 usage，计价拿不到数
    body["stream_options"] = nlohmann::json{{"include_usage", true}};
    if (const std::string effort = d.value("effort", std::string()); !effort.empty())
        body["reasoning_effort"] = effort;

    nlohmann::json msgs = nlohmann::json::array();
    if (const std::string system = d.value("system", std::string()); !system.empty())
        msgs.push_back(nlohmann::json{{"role", "system"}, {"content", system}});

    if (d.contains("messages") && d["messages"].is_array())
    {
        for (const nlohmann::json &m : d["messages"])
        {
            const std::string role = m.at("role");
            const nlohmann::json blocks = m.value("content", nlohmann::json::array());

            // tool_result 各自单独成一条 role=tool 的 message，先把它们摘出去
            nlohmann::json tool_msgs = nlohmann::json::array();
            nlohmann::json tool_calls = nlohmann::json::array();
            for (const nlohmann::json &b : blocks)
            {
                const std::string bt = b.at("type");
                if (bt == "tool_result")
                {
                    tool_msgs.push_back(nlohmann::json{{"role", "tool"},
                                                       {"tool_call_id", b.at("tool_use_id")},
                                                       {"content", tool_content_text(b.at("content"))}});
                }
                else if (bt == "tool_use")
                {
                    nlohmann::json fn;
                    fn["name"] = b.at("name");
                    // 参数是 JSON 字符串，不是对象——本协议如此
                    fn["arguments"] = b.value("input", nlohmann::json::object()).dump();
                    tool_calls.push_back(
                        nlohmann::json{{"id", b.at("id")}, {"type", "function"}, {"function", fn}});
                }
            }

            const std::string text = join_text(blocks);
            if (!text.empty() || !tool_calls.empty())
            {
                nlohmann::json mout;
                mout["role"] = role == "user" ? "user" : "assistant";
                mout["content"] = text;
                if (!tool_calls.empty()) mout["tool_calls"] = tool_calls;
                msgs.push_back(mout);
            }
            for (nlohmann::json &tm : tool_msgs) msgs.push_back(tm);
        }
    }
    body["messages"] = msgs;

    if (d.contains("tools") && d["tools"].is_array())
    {
        nlohmann::json tools = nlohmann::json::array();
        for (const nlohmann::json &t : d["tools"])
        {
            nlohmann::json fn;
            fn["name"] = t.at("name");
            if (t.contains("description")) fn["description"] = t.at("description");
            fn["parameters"] = t.value("input_schema", nlohmann::json::object());
            tools.push_back(nlohmann::json{{"type", "function"}, {"function", fn}});
        }
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/chat/completions";
    if (const std::string key = cfg.get("api_key"); !key.empty())
        req.headers.push_back("Authorization: Bearer " + key);
    req.headers.push_back("Content-Type: application/json");
    req.body = body.dump();
    return req;
}

} // namespace realagent
