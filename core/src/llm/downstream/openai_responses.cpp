/*
 * downstream/openai_responses.cpp — /responses 的 SSE 帧 → 事件
 *
 * 与另外两套的实质差异：
 *   1. 帧类型在 SSE 的 event: 行上，data 里是该类型自己的强类型载荷
 *      （另外两套都是 data 里带 type 字段）——这就是 split_sse_block 要把
 *      event: 行也留下来的原因
 *   2. 工具调用有明确的开始（response.output_item.added）与结束
 *      （response.output_item.done）帧，不必像 openai_chat 那样按 index 攒
 *   3. usage 在 response.completed 的 response.usage 里，字段是
 *      input_tokens / output_tokens，缓存命中在 input_tokens_details.cached_tokens
 *   4. 失败是一个显式的帧（response.failed / error），不是 HTTP 状态——
 *      它必须让本次调用失败，不能当成"没内容"
 */
#include <cstdio>
#include <exception>

#include "llm/llm.hpp"

namespace realagent {

bool feed_block(protocol::OpenAiResponses, OpenAiResponsesState &st, std::string_view block,
                const EventSink &sink)
{
    const SseBlock sb = split_sse_block(block);
    if (sb.data.empty()) return true;
    try
    {
        const nlohmann::json o = nlohmann::json::parse(sb.data, nullptr, false);
        if (o.is_discarded() || !o.is_object()) return true;
        // 类型优先取 event: 行；有些实现在 data 里也带一份 type，两处一致时取哪个都一样
        const std::string t = !sb.event.empty() ? sb.event : json_str(o, "type");

        if (t == "response.output_text.delta")
        {
            close_thinking(st.reasoning_open, sink);
            if (sink) sink("message_update", nlohmann::json{{"delta", json_str(o, "delta")}});
        }
        else if (t == "response.reasoning_summary_text.delta" ||
                 t == "response.reasoning_text.delta")
        {
            if (sink)
            {
                if (!st.reasoning_open)
                {
                    st.reasoning_open = true;
                    // 本协议不给 signature，字段留着让上层一视同仁
                    sink("thinking_start", nlohmann::json{{"signature", ""}});
                }
                sink("thinking_update", nlohmann::json{{"delta", json_str(o, "delta")}});
            }
        }
        else if (t == "response.reasoning_summary_text.done" ||
                 t == "response.reasoning_text.done")
        {
            close_thinking(st.reasoning_open, sink);
        }
        else if (t == "response.output_item.added")
        {
            if (const auto item = o.find("item"); item != o.end() && item->is_object())
            {
                if (json_str(*item, "type") == "function_call")
                {
                    close_thinking(st.reasoning_open, sink);
                    st.tool_id_ = json_str(*item, "call_id");
                    st.tool_name_ = json_str(*item, "name");
                    st.tool_input_.clear();
                }
            }
        }
        else if (t == "response.function_call_arguments.delta")
        {
            st.tool_input_ += json_str(o, "delta");
        }
        else if (t == "response.output_item.done")
        {
            const auto item = o.find("item");
            if (item == o.end() || !item->is_object()) return true;
            if (json_str(*item, "type") != "function_call") return true;
            // 结束帧带全量 arguments 就用它，只有增量就用攒的——两者应当一致
            const std::string args =
                !json_str(*item, "arguments").empty() ? json_str(*item, "arguments") : st.tool_input_;
            if (sink)
            {
                nlohmann::json in = nlohmann::json::parse(args.empty() ? "{}" : args, nullptr, false);
                sink("tool_use",
                     nlohmann::json{{"id", st.tool_id_.empty() ? json_str(*item, "call_id") : st.tool_id_},
                                    {"name", st.tool_name_.empty() ? json_str(*item, "name") : st.tool_name_},
                                    {"input", in.is_discarded() ? nlohmann::json::object() : in}});
            }
            st.tool_id_.clear();
            st.tool_name_.clear();
            st.tool_input_.clear();
            st.finish_reason = "tool_use";
        }
        else if (t == "response.completed" || t == "response.incomplete")
        {
            close_thinking(st.reasoning_open, sink);
            if (const auto r = o.find("response"); r != o.end() && r->is_object())
            {
                if (const auto u = r->find("usage"); u != r->end() && u->is_object())
                {
                    if (const long long n = json_int(*u, "input_tokens"); n > 0) st.usage.input = n;
                    if (const long long n = json_int(*u, "output_tokens"); n > 0) st.usage.output = n;
                    if (const auto d = u->find("input_tokens_details");
                        d != u->end() && d->is_object())
                    {
                        if (const long long n = json_int(*d, "cached_tokens"); n > 0)
                            st.usage.cache_read = n;
                    }
                    emit_usage(st.usage, sink);
                }
            }
            if (sink)
                sink("stop",
                     nlohmann::json{{"reason", st.finish_reason.empty() ? "stop" : st.finish_reason}});
            st.finish_reason.clear();
        }
        else if (t == "response.failed" || t == "error")
        {
            // 显式失败帧：HTTP 是 200，但这次调用没成。绝不能当成"没内容"
            fprintf(stderr, "[llm] openai-responses 失败帧: %.200s\n", sb.data.c_str());
            return false;
        }
        return true;
    } catch (const std::exception &e)
    {
        fprintf(stderr, "[llm] openai-responses 帧不合规: %s | data=%.200s\n", e.what(),
                sb.data.c_str());
    } catch (...)
    {
        fprintf(stderr, "[llm] openai-responses 帧不合规（未知异常）| data=%.200s\n",
                sb.data.c_str());
    }
    return false;
}

} // namespace realagent
