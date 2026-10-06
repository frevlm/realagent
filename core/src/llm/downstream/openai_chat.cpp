/*
 * downstream/openai_chat.cpp — /chat/completions 的 SSE 帧 → 事件
 *
 * 与 anthropic_messages 的实质差异：
 *   1. 流的终点是一行字面量 `data: [DONE]`，不是一个带类型的帧
 *   2. 工具调用按 index 增量到达（id 与 name 只在首帧给，arguments 分片累积），
 *      所以要按 index 攒；攒好的在收工那一帧一次性发出，顺序按首次出现
 *   3. usage 只在最后一个 choices 为空的帧里给（还得请求时开 stream_options）
 *   4. token 字段叫 prompt_tokens / completion_tokens，缓存命中藏在
 *      prompt_tokens_details.cached_tokens 里——换算在这儿做完，出门一律叫
 *      input / output / cache_read
 *   5. 思考内容叫 reasoning_content（DeepSeek）或 reasoning，没有 signature，
 *      也没有"块结束"帧——thinking_stop 由本文件在正文开始或收工时补
 */
#include <cstdio>
#include <exception>

#include "llm/llm.hpp"

namespace realagent {

namespace {

/* 收工：先把攒好的工具调用发出去，再发 stop。顺序不能反——
 * 上层收到 stop 就当本轮结束了，之后来的 tool_use 没人接。 */
void finish(OpenAiChatState &st, const EventSink &sink)
{
    close_thinking(st.reasoning_open, sink);
    if (sink)
    {
        for (const long long idx : st.tool_order)
        {
            const auto it = st.tools.find(idx);
            if (it == st.tools.end()) continue;
            nlohmann::json in = nlohmann::json::parse(it->second.args.empty() ? "{}" : it->second.args, nullptr, false);
            sink("tool_use", nlohmann::json{{"id", it->second.id},
                                            {"name", it->second.name},
                                            {"input", in.is_discarded() ? nlohmann::json::object() : in}});
        }
        nlohmann::json ev;
        // 本协议的 tool_calls 与 anthropic 的 tool_use 是同一件事，收工理由也归一
        ev["reason"] = st.finish_reason == "tool_calls" ? "tool_use"
                       : st.finish_reason.empty()       ? "stop"
                                                        : st.finish_reason;
        sink("stop", ev);
    }
    st.tools.clear();
    st.tool_order.clear();
    st.finish_reason.clear();
}

} // namespace

bool feed_block(protocol::OpenAiChat, OpenAiChatState &st, std::string_view block,
                const EventSink &sink)
{
    const SseBlock sb = split_sse_block(block);
    if (sb.data.empty()) return true;
    if (sb.data == "[DONE]")
    {
        // 有些端点只给 [DONE] 不给 finish_reason：收工帧照发，不让上层空等
        if (!st.tool_order.empty() || !st.finish_reason.empty() || st.reasoning_open)
            finish(st, sink);
        return true;
    }
    try
    {
        const nlohmann::json o = nlohmann::json::parse(sb.data, nullptr, false);
        // 不是 JSON 的 data 行：忽略，不是错
        if (o.is_discarded() || !o.is_object()) return true;

        // 端点把错误塞进流里（HTTP 200 + 一帧 error）：这不是内容，是失败
        if (o.contains("error"))
        {
            fprintf(stderr, "[llm] openai-chat 流内错误: %.200s\n", sb.data.c_str());
            return false;
        }

        if (const auto u = o.find("usage"); u != o.end() && u->is_object())
        {
            if (const long long n = json_int(*u, "prompt_tokens"); n > 0) st.usage.input = n;
            if (const long long n = json_int(*u, "completion_tokens"); n > 0) st.usage.output = n;
            if (const auto pd = u->find("prompt_tokens_details");
                pd != u->end() && pd->is_object())
            {
                if (const long long n = json_int(*pd, "cached_tokens"); n > 0)
                    st.usage.cache_read = n;
            }
            emit_usage(st.usage, sink);
        }

        const auto choices = o.find("choices");
        if (choices == o.end() || !choices->is_array() || choices->empty()) return true;
        const nlohmann::json &c0 = (*choices)[0];

        if (const auto delta = c0.find("delta"); delta != c0.end() && delta->is_object())
        {
            const nlohmann::json &d = *delta;

            // 思考内容：两个字段名都认，同一件事
            std::string reasoning = json_str(d, "reasoning_content");
            if (reasoning.empty()) reasoning = json_str(d, "reasoning");
            if (!reasoning.empty() && sink)
            {
                if (!st.reasoning_open)
                {
                    st.reasoning_open = true;
                    // 本协议没有 signature，字段留着让上层一视同仁
                    sink("thinking_start", nlohmann::json{{"signature", ""}});
                }
                sink("thinking_update", nlohmann::json{{"delta", reasoning}});
            }

            if (const std::string text = json_str(d, "content"); !text.empty())
            {
                close_thinking(st.reasoning_open, sink); // 正文开始 = 思考结束，本协议不另发结束帧
                if (sink) sink("message_update", nlohmann::json{{"delta", text}});
            }

            if (const auto tcs = d.find("tool_calls"); tcs != d.end() && tcs->is_array())
            {
                close_thinking(st.reasoning_open, sink);
                for (const nlohmann::json &tc : *tcs)
                {
                    const long long idx = json_int(tc, "index");
                    auto [it, fresh] = st.tools.try_emplace(idx);
                    if (fresh) st.tool_order.push_back(idx);
                    if (const std::string id = json_str(tc, "id"); !id.empty()) it->second.id = id;
                    if (const auto fn = tc.find("function"); fn != tc.end() && fn->is_object())
                    {
                        if (const std::string n = json_str(*fn, "name"); !n.empty())
                            it->second.name = n;
                        it->second.args += json_str(*fn, "arguments");
                    }
                }
            }
        }

        if (const std::string fr = json_str(c0, "finish_reason"); !fr.empty())
        {
            st.finish_reason = fr;
            finish(st, sink);
        }
        return true;
    } catch (const std::exception &e)
    {
        fprintf(stderr, "[llm] openai-chat 帧不合规: %s | data=%.200s\n", e.what(),
                sb.data.c_str());
    } catch (...)
    {
        fprintf(stderr, "[llm] openai-chat 帧不合规（未知异常）| data=%.200s\n", sb.data.c_str());
    }
    return false;
}

} // namespace realagent
