/*
 * context_session.cpp — 增量上下文协议的客户端（协议契约见 revlm docs/context-cache.md）
 */
#include "llm/context_session.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cstdio>

#include "json.hpp"

namespace realagent {

std::string ContextSession::sha256_hex(std::string_view data)
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_Digest(data.data(), data.size(), md, &n, EVP_sha256(), nullptr);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (unsigned int i = 0; i < n; ++i)
    {
        out.push_back(kHex[md[i] >> 4]);
        out.push_back(kHex[md[i] & 15]);
    }
    return out;
}

ContextSession::Keep ContextSession::diff(std::string_view p, std::string_view b)
{
    const std::size_t lim = std::min(p.size(), b.size());
    std::size_t head = 0;
    while (head < lim && p[head] == b[head]) ++head;
    // tail 只在 head 之后的部分里找，保证 head + tail <= min(|p|, |b|)
    const std::size_t room = lim - head;
    std::size_t tail = 0;
    while (tail < room && p[p.size() - 1 - tail] == b[b.size() - 1 - tail]) ++tail;
    return {head, tail};
}

ContextSession::Wire ContextSession::as_full()
{
    delta_ = false;
    sent_ = body_.size();
    return {body_, {"X-Revlm-Expect: " + body_hex_}};
}

ContextSession::Wire ContextSession::full()
{
    stats_.misses++;
    return as_full();
}

ContextSession::Wire ContextSession::prepare(const std::string &full_body)
{
    body_ = full_body;
    body_hex_ = sha256_hex(body_);
    stats_.requests++;
    const Keep k = base_.empty() ? Keep{} : diff(base_, body_);
    if (k.head + k.tail < kMinSaving) return as_full();

    delta_ = true;
    stats_.deltas++;
    Wire w{body_.substr(k.head, body_.size() - k.head - k.tail),
           {"X-Revlm-Expect: " + body_hex_, "X-Revlm-Base: " + base_hex_,
            "X-Revlm-Keep: " + std::to_string(k.head) + "," + std::to_string(k.tail)}};
    sent_ = w.body.size();
    return w;
}

bool ContextSession::is_conflict(long status, const std::string &error_body)
{
    if (status != 409) return false;
    const nlohmann::json j = nlohmann::json::parse(error_body, nullptr, false);
    if (!j.is_object() || !j.contains("error") || !j["error"].is_object()) return false;
    const nlohmann::json t = j["error"].value("type", nlohmann::json());
    return t == "revlm_base_miss" || t == "revlm_expect_mismatch";
}

void ContextSession::on_response(const std::string &stored)
{
    if (delta_)
    {
        stats_.hits++;
        stats_.bytes_saved += static_cast<long long>(body_.size()) - static_cast<long long>(sent_);
    }
    delta_ = false;
    if (stored == body_hex_ && !stored.empty())
    {
        base_ = std::move(body_);
        base_hex_ = stored;
    }
}

} // namespace realagent
