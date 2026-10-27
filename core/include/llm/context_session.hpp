/*
 * context_session.hpp — 增量上下文协议的客户端会话（revlm docs/context-cache.md）
 *
 * 把请求体当字节：新请求体 B 与上一次被服务端存下的请求体 P 比较，
 *   B = P[0:head] + middle + P[|P|-tail:]
 * head 是最长公共前缀，tail 是其余部分的最长公共后缀，只发 middle。协议与端点无关，
 * anthropic-messages / openai-chat / openai-responses 一视同仁；请求体由 build_request
 * 照旧构造，这里不解析、不重排。
 *
 * 只做协议与状态，不管传输：llm_call 把发送体与头交给 llm/transport、取响应头即可，
 * TUI/GUI 调用方零改动。一个 Agent 一份，只在它自己的循环线程上动，不加锁。
 * 收工判定那次调用不走它——每次判定都是一段单条消息的新对话，没有可比的 P。
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace realagent {

class ContextSession {
  public:
    /* 省下的字节不到这个数就发完整体：增量的头开销与服务端拼装不划算。 */
    static constexpr std::size_t kMinSaving = 4096;

    /* 要发出去的东西：请求体 + 追加在 build_request 那组头之后的协议头。 */
    struct Wire {
        std::string body;
        std::vector<std::string> headers; // "Key: value"
    };

    /* requests = 走协议的请求数；deltas = 发出的增量体；hits = 增量被接受（2xx）；
     * misses = 409 后的完整重发；bytes_saved = 命中时累加 |B| - |实际发送体|。 */
    struct Stats {
        int requests = 0;
        int deltas = 0;
        int hits = 0;
        int misses = 0;
        long long bytes_saved = 0;
    };

    /* P 保留 head 字节开头、tail 字节结尾。 */
    struct Keep {
        std::size_t head = 0;
        std::size_t tail = 0;
    };

    /* build_request 造好的完整体 B → 发送体。记住 B 与 sha256(B)；有已存的 P 且省下
     * >= kMinSaving 字节发增量，否则发完整体。 */
    Wire prepare(const std::string &full_body);

    /* 409 后重发：最近一次 prepare 的 B 的完整模式。 */
    Wire full();

    /* 409 且错误体是 revlm_base_miss / revlm_expect_mismatch：上游还没被调用、不计
     * 费，调用方应立即 full() 重发。其余 409 是真错误，照常报。 */
    static bool is_conflict(long status, const std::string &error_body);

    /* 成功响应的 X-Revlm-Stored 头值（可为空）。等于 sha256(B) → B 成为新的 P：只有
     * 服务端确认存下的请求体才配当 base。增量命中在这里落账。 */
    void on_response(const std::string &stored);

    /* p 与 b 的 head/tail，满足 head + tail <= min(|p|, |b|)。 */
    static Keep diff(std::string_view p, std::string_view b);

    static std::string sha256_hex(std::string_view data);

    const Stats &stats() const { return stats_; }

  private:
    Wire as_full();

    std::string base_;     // P；空 = 还没有被存下的请求体
    std::string base_hex_; // sha256(P)
    std::string body_;     // 最近一次 prepare 的 B
    std::string body_hex_; // sha256(B)
    std::size_t sent_ = 0; // 本轮实际发出的请求体长度
    bool delta_ = false;   // 本轮发的是增量体（409 重发后清掉）
    Stats stats_;
};

} // namespace realagent
