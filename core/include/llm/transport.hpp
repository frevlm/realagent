/*
 * transport.hpp — 把一个 HttpRequest 发出去，响应体分块交回来
 *
 * 两条路，签名相同：
 *   post_revlm  revlm::net（revlm 自己的网络层：h2 / h2c / realurl/1），只用来连 revlm
 *   post_curl   libcurl，什么端点都能连
 * 连 revlm（protocol=revlm）走 post_revlm，其余走 post_curl。对面说不了 h2
 * （例如 revlm 前面挡着只认 HTTP/1.1 的反向代理）时 post_revlm 在拿到响应头之前就失败：
 * 建连失败，或对面回的第一帧就不合 h2——请求没被当成请求处理过，同一个请求退回 post_curl。
 */
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

#include "llm/llm.hpp"

namespace realagent {

/* 2xx 响应体的一块。返回 false = 解析失败，中止传输。 */
using BodySink = std::function<bool(std::string_view chunk)>;

struct PostResult {
    long status = 0;          // 0 = 没拿到响应
    std::string error_body;   // 非 2xx 时的响应体（只留前 8 KiB）
    std::string revlm_stored; // 响应头 X-Revlm-Stored（增量上下文协议，可空）
    bool sink_failed = false; // BodySink 返回过 false
    std::string error;        // 传输错误（含用户中断）；空 = 传输本身没出错
};

/* 阻塞到响应读完、出错或 abort 变 true。 */
PostResult post_curl(const HttpRequest &req, const BodySink &sink, const std::atomic<bool> &abort);
PostResult post_revlm(const HttpRequest &req, const BodySink &sink, const std::atomic<bool> &abort);

} // namespace realagent
