/*
 * context_session.hpp — 增量上下文协议的客户端会话（revlm docs/context-cache.md）
 *
 * 客户端本地保留完整历史，每轮只把新增消息发给 revlm，revlm 从盘上拼回完整请求
 * 转发上游。链条：blob(m)=SHA-256(canonical(m))，node(0)=SHA-256("revlm-ctx-v1")，
 * node(k)=SHA-256(node(k-1)‖blob(m_k))——id 相同即前 k 条逐字节相同；对不上就 409，
 * 客户端立即重发完整请求，不会拼错。canonical 与拼装全在 realurl::llm 共享模块里，
 * 两端同一份实现，字节级一致才有意义。
 *
 * 只做协议与状态，不管传输：llm_call 里那条 curl 路径注入发送体、取响应即可，
 * TUI/GUI 调用方零改动。一个 Agent 一份，只在它自己的循环线程上动，不加锁。
 * 收工判定那次调用不走它——每次判定都是一段单条消息的新对话，接不上任何链。
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "json.hpp"
#include "realurl/llm/context_canonical.hpp"

namespace realagent {

class ContextSession {
  public:
    ContextSession() { nodes_.push_back(realurl::llm::root_node()); }
    /* 命中/未命中与省下的上传字节（完整体大小 - 实际发送体大小，命中时累加）。 */
    struct Stats {
        int incrementals = 0; // 发出的增量体
        int hits = 0;         // 增量被服务端接受（2xx 响应）
        int misses = 0;       // 409 revlm_context_miss / revlm_context_mismatch
        long long bytes_saved = 0;
    };

    /* 完整请求体（upstream 造好的 /v1/messages body）→ 发送体。
     * 无已知前缀 / 历史对不上 / shape 变了 → revlm_context:{} 的完整体；否则增量体
     * （messages 只带尾部）。本地链同步到本次请求的完整消息，延伸一次算一次。 */
    std::string wrap_request(const std::string &full_request_body);

    /* 409 且错误体是 revlm_context_miss / revlm_context_mismatch：上游还没被调
     * 用、不计费，调用方应立即重发完整请求。其余 409 是真错误，照常报。 */
    static bool is_conflict(long status, const std::string &error_body);

    /* 409 后重发的完整体（revlm_context:{}）；已知前缀随之作废——完整请求会重新
     * 建立 revlm 那头的链，残余的不一致下一轮表现为 miss、自愈。 */
    std::string resend_full();

    /* 成功响应的 X-Revlm-Context 头（"prefix=<hex>; count=<n>; shape=<hex>"，可为
     * 空）。服务端 prefix 与本地不同只出现在服务端重组不一致时：以本地为准，不拿
     * 服务端的覆盖本地。命中统计在这里落账。 */
    void on_response(const std::string &header_value);

    /* 一次成功调用后，把收到的 assistant 消息（抽象对话形状）接上链：下一轮的
     * prefix 就是这颗 node(n+1)。块序与 build_request 同一条转换，保证与下一轮
     * 请求里的字节一致。 */
    void record_assistant(const nlohmann::json &message);

    const Stats &stats() const { return stats_; }

  private:
    std::string wrap_throws(const std::string &full_request_body);

    std::vector<std::string> blobs_;        // canonical(m)，cache_control 剥掉后
    std::vector<realurl::llm::Node> nodes_; // nodes_[0]=root，nodes_[k]=第 k 条后的链头
    std::string shape_hex_;                 // 最近一次请求的 shape；空 = 还没发过
    std::string full_wire_;                 // 最近一次 wrap 的完整发送体（409 重发用）
    std::string inc_wire_;                  // 最近一次 wrap 的增量发送体；空 = 这次是完整体
    long long pending_save_ = 0;            // 这一轮命中才能落账的节字节数
    Stats stats_;
};

} // namespace realagent
