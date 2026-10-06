/*
 * downstream/anthropic_messages.cpp — /v1/messages 的 SSE 帧 → 事件
 *
 * 帧类型在 data 的 type 字段里（event: 行与它永远一致，取两遍就得处理它们不一致时
 * 听谁的，那是凭空造出来的问题）。
 *
 * usage：各家在不同帧里给不同字段（input 在 message_start，output 在 message_delta），
 * 因此计数先落在状态上、合并后整体发出——下游永远收到完整一组，
 * 不必猜"0 是真的 0 还是这帧没给"。
 *
 * 按帧结构直取字段（.at 与隐式转换均会抛），帧一旦不合规就抛异常——
 * 就地兜住转成 false。异常绝不能穿出去：本函数由 libcurl 的写回调调进来，
 * 中间隔着 C 栈帧，异常穿过去是未定义行为。
 */
#include <cstdio>
#include <exception>

#include "llm/llm.hpp"

namespace realagent {

namespace {

/* 合并一帧 usage 并发出。绝对值：后到覆盖先到，丢帧不造成永久偏差 */
void merge_usage(const nlohmann::json &u, UsageCounts &c, const EventSink &sink)
{
    if (json_int(u, "input_tokens") > 0) c.input = json_int(u, "input_tokens");
    if (json_int(u, "output_tokens") > 0) c.output = json_int(u, "output_tokens");
    if (json_int(u, "cache_read_input_tokens") > 0)
        c.cache_read = json_int(u, "cache_read_input_tokens");
    if (json_int(u, "cache_creation_input_tokens") > 0)
        c.cache_write = json_int(u, "cache_creation_input_tokens");
    emit_usage(c, sink);
}

} // namespace

bool feed_block(protocol::AnthropicMessages, AnthropicState &st, std::string_view block,
                const EventSink &sink)
{
    const SseBlock sb = split_sse_block(block);
    if (sb.data.empty()) return true;
    try
    {
        const nlohmann::json o = nlohmann::json::parse(sb.data, nullptr, false);
        // 不是 JSON 的 data 行（如 [DONE]）：忽略，不是错
        if (o.is_discarded() || !o.is_object()) return true;
        const std::string t = o.at("type");

        if (t == "message_start")
        {
            // 新 message：计数清零，再合并首帧 usage（input_tokens 在这里给）
            st.usage = UsageCounts{};
            const auto msg = o.find("message");
            if (msg != o.end() && msg->is_object())
            {
                const auto u = msg->find("usage");
                if (u != msg->end() && u->is_object()) merge_usage(*u, st.usage, sink);
            }
        }
        else if (t == "content_block_start")
        {
            const nlohmann::json &cb = o.at("content_block");
            const std::string cbt = cb.at("type");
            st.block_type_ = cbt;
            if (cbt == "tool_use")
            {
                st.tool_id_ = cb.at("id");
                st.tool_name_ = cb.at("name");
                st.tool_input_.clear();
            }
            else if (cbt == "thinking")
            {
                // 思考块开始：先发 signature，再发起始文本（部分端点起始块自带文本）
                st.thinking_sig_ = cb.value("signature", st.thinking_sig_);
                if (sink) sink("thinking_start", nlohmann::json{{"signature", st.thinking_sig_}});
                if (const std::string init = cb.value("thinking", std::string());
                    !init.empty() && sink)
                    sink("thinking_update", nlohmann::json{{"delta", init}});
            }
        }
        else if (t == "content_block_delta")
        {
            const nlohmann::json &delta = o.at("delta");
            const std::string dt = delta.at("type");
            if (dt == "text_delta" && sink)
            {
                sink("message_update", nlohmann::json{{"delta", delta.at("text")}});
            }
            else if (dt == "thinking_delta" && st.block_type_ == "thinking" && sink)
            {
                sink("thinking_update", nlohmann::json{{"delta", delta.at("thinking")}});
            }
            else if (dt == "input_json_delta" && st.block_type_ == "tool_use")
            {
                st.tool_input_ += delta.at("partial_json").get<std::string>();
            }
        }
        else if (t == "content_block_stop")
        {
            if (st.block_type_ == "tool_use" && sink)
            {
                nlohmann::json in = nlohmann::json::parse(st.tool_input_, nullptr, false);
                sink("tool_use", nlohmann::json{{"id", st.tool_id_},
                                                {"name", st.tool_name_},
                                                {"input", in.is_discarded() ? nlohmann::json::object() : in}});
                st.block_type_.clear();
            }
            else if (st.block_type_ == "thinking")
            {
                if (sink) sink("thinking_stop", nlohmann::json::object());
                st.block_type_.clear();
                st.thinking_sig_.clear();
            }
        }
        else if (t == "message_delta")
        {
            // output_tokens 在这里给终值：先发 usage 再发 stop，保证下游收工时数字已定
            if (const auto u = o.find("usage"); u != o.end() && u->is_object())
                merge_usage(*u, st.usage, sink);
            if (sink)
            {
                // delta 缺失/非对象不是丢帧的理由：stop 照发，reason 退回默认值
                nlohmann::json ev;
                ev["reason"] = "stop";
                if (const auto dd = o.find("delta"); dd != o.end() && dd->is_object())
                    if (dd->contains("stop_reason")) ev["reason"] = (*dd)["stop_reason"];
                sink("stop", ev);
            }
        }
        return true;
    } catch (const std::exception &e)
    {
        fprintf(stderr, "[llm] anthropic-messages 帧不合规: %s | data=%.200s\n", e.what(),
                sb.data.c_str());
    } catch (...)
    {
        fprintf(stderr, "[llm] anthropic-messages 帧不合规（未知异常）| data=%.200s\n",
                sb.data.c_str());
    }
    return false;
}

} // namespace realagent
