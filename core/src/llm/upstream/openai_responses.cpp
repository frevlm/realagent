/*
 * upstream/openai_responses.cpp — 抽象对话 → /responses 请求
 *
 * 与 openai_chat 的差异（同一家公司的两套协议，不是同一套的两个版本）：
 *   1. 对话叫 input 不叫 messages，条目是强类型 item：message / function_call /
 *      function_call_output 三种，各有各的形状
 *   2. 文本块按方向分名：用户侧 input_text，助手侧 output_text
 *   3. 工具是平的（type/name/parameters 同层），不套一层 function
 *   4. system 走顶层 instructions
 *   5. 工具结果的关联键叫 call_id，不叫 tool_call_id
 */
#include "llm/llm.hpp"
#include "tools/tools.hpp"

namespace realagent {

HttpRequest build_request(protocol::OpenAiResponses, const Config &cfg, const nlohmann::json &dialog)
{
    const nlohmann::json &d = dialog;

    nlohmann::json body;
    body["model"] = d.value("model", std::string());
    body["stream"] = true;
    if (const std::string system = d.value("system", std::string()); !system.empty())
        body["instructions"] = system;
    // 不要 summary 就收不到 reasoning_summary_text.delta，💭 画不出来
    if (const std::string effort = d.value("effort", std::string()); !effort.empty())
        body["reasoning"] = nlohmann::json{{"effort", effort}, {"summary", "auto"}};

    nlohmann::json input = nlohmann::json::array();
    if (d.contains("messages") && d["messages"].is_array())
    {
        for (const nlohmann::json &m : d["messages"])
        {
            const std::string role = m.at("role");
            const bool is_user = role == "user";
            const nlohmann::json blocks = m.value("content", nlohmann::json::array());

            nlohmann::json text_parts = nlohmann::json::array();
            for (const nlohmann::json &b : blocks)
            {
                const std::string bt = b.at("type");
                if (bt == "text")
                {
                    text_parts.push_back(nlohmann::json{{"type", is_user ? "input_text" : "output_text"},
                                                        {"text", b.at("text")}});
                }
                else if (bt == "tool_use")
                {
                    input.push_back(nlohmann::json{{"type", "function_call"},
                                                   {"call_id", b.at("id")},
                                                   {"name", b.at("name")},
                                                   {"arguments", b.value("input", nlohmann::json::object()).dump()}});
                }
                else if (bt == "tool_result")
                {
                    input.push_back(nlohmann::json{{"type", "function_call_output"},
                                                   {"call_id", b.at("tool_use_id")},
                                                   {"output", tool_content_text(b.at("content"))}});
                }
            }
            if (!text_parts.empty())
                input.push_back(
                    nlohmann::json{{"type", "message"}, {"role", role}, {"content", text_parts}});
        }
    }
    body["input"] = input;

    if (d.contains("tools") && d["tools"].is_array())
    {
        nlohmann::json tools = nlohmann::json::array();
        for (const nlohmann::json &t : d["tools"])
        {
            nlohmann::json tool;
            tool["type"] = "function";
            tool["name"] = t.at("name");
            if (t.contains("description")) tool["description"] = t.at("description");
            tool["parameters"] = t.value("input_schema", nlohmann::json::object());
            tools.push_back(tool);
        }
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    HttpRequest req;
    req.url = cfg.get("base_url") + "/responses";
    if (const std::string key = cfg.get("api_key"); !key.empty())
        req.headers.push_back("Authorization: Bearer " + key);
    req.headers.push_back("Content-Type: application/json");
    req.body = body.dump();
    return req;
}

} // namespace realagent
