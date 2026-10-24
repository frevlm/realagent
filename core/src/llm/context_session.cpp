/*
 * context_session.cpp — 增量上下文协议的客户端（协议契约见 revlm docs/context-cache.md）
 *
 * 发送体的构造规则（与服务端同一套）：
 *   完整请求对象 → 剥 revlm_context → strip_cache_control 记 marks → 逐条 canonical
 *   得 blobs；完整拼装体 = assemble_request_body(request, blobs, marks)，expect 是
 *   它的 SHA-256。增量体 = 同一请求对象、messages 换尾部、revlm_context 填
 *   prefix/count/shape/cache_control（marks 全量，路径按完整请求计）/expect。
 */
#include "llm/context_session.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <charconv>
#include <cstdio>
#include <exception>
#include <variant>

#include "llm/llm.hpp"

namespace realagent {
namespace rllm = realurl::llm;

namespace {

/* marks（CacheMark/RequestMark 同形）→ revlm_context.cache_control 的 JSON 形状 */
boost::json::value marks_to_json(const std::vector<rllm::RequestMark> &marks)
{
    boost::json::array out;
    for (const auto &m : marks)
    {
        boost::json::array path;
        for (const auto &el : m.path)
        {
            if (const auto *key = std::get_if<std::string>(&el))
                path.push_back(boost::json::value(*key));
            else
                path.push_back(boost::json::value(std::get<std::uint64_t>(el)));
        }
        boost::json::object o;
        o["path"] = std::move(path);
        o["value"] = m.value;
        out.push_back(std::move(o));
    }
    return out;
}

/* 请求里的 system / tools（剥过 cache_control 的），缺省为 null —— shape 只认这两样 */
boost::json::value member_or_null(const boost::json::object &o, const char *key)
{
    const auto it = o.find(key);
    return it == o.end() ? boost::json::value{} : it->value();
}

} // namespace

std::string ContextSession::wrap_request(const std::string &full_request_body)
{
    try
    {
        return wrap_throws(full_request_body);
    } catch (const std::exception &e)
    {
        // 协议坏了不能坏了调用：退回原始完整体（连 revlm_context 都不带，当普通
        // 请求发），下一轮从完整请求重新建立链。
        fprintf(stderr, "[llm] revlm 上下文请求构造失败，本次退回完整请求：%s\n", e.what());
        inc_wire_.clear();
        pending_save_ = 0;
        full_wire_ = full_request_body;
        return full_request_body;
    }
}

std::string ContextSession::wrap_throws(const std::string &full_request_body)
{
    boost::json::value request = boost::json::parse(full_request_body);
    boost::json::object &ro = request.as_object();
    ro.erase("revlm_context"); // 本方不会放；按契约先剥掉再算
    // strip 出来的是 CacheMark，路径相对剥掉后的请求根——正是 RequestMark 要的形状
    std::vector<rllm::RequestMark> marks;
    for (const rllm::CacheMark &m : rllm::strip_cache_control(request))
        marks.push_back({m.path, m.value});
    const boost::json::array &msgs = ro.at("messages").as_array();

    std::vector<std::string> blobs;
    blobs.reserve(msgs.size());
    for (const auto &m : msgs) blobs.push_back(rllm::canonical_message(m));

    // 完整拼装体与 expect：两端各自动手、必须逐字节相同，所以都走共享模块
    const std::string full_assembled = rllm::assemble_request_body(request, blobs, marks);
    const std::string expect = rllm::sha256_hex(full_assembled);
    const std::string shape =
        rllm::to_hex(rllm::shape_hash(member_or_null(ro, "system"), member_or_null(ro, "tools")));

    // 本地链对齐本次请求：公共前缀保留，其后截断、新消息逐条延伸——回退与改写
    // 走的是同一条路，不是特殊情况。
    std::size_t common = 0;
    while (common < blobs_.size() && common < blobs.size() && blobs_[common] == blobs[common])
        ++common;
    const bool prefix_ok = !blobs_.empty() && common == blobs_.size() && shape_hex_ == shape;
    blobs_.resize(common);
    nodes_.resize(common + 1);
    for (std::size_t i = common; i < blobs.size(); ++i)
    {
        blobs_.push_back(blobs[i]);
        nodes_.push_back(rllm::chain_node(nodes_.back(), rllm::blob_hash(msgs[i])));
    }
    shape_hex_ = shape;

    // 尾部消息先摘出来：object 插入新键会让成员搬家，msgs 这个引用不能跨过
    // 下面的改动再接着用。
    boost::json::array tail;
    for (std::size_t i = common; i < msgs.size(); ++i) tail.push_back(msgs[i]);

    // 完整发送体：同一请求对象 + revlm_context:{}
    ro["revlm_context"] = boost::json::object{};
    full_wire_ = boost::json::serialize(request);
    if (!prefix_ok)
    {
        inc_wire_.clear();
        pending_save_ = 0;
        return full_wire_;
    }

    // 增量发送体：messages 换尾部，revlm_context 填协议字段；marks 的路径本来就是
    // 按完整请求计的。expect 仍是对完整拼装体的哈希。
    boost::json::object ctx;
    ctx["prefix"] = rllm::to_hex(nodes_[common]);
    ctx["count"] = static_cast<std::uint64_t>(common);
    ctx["shape"] = shape_hex_;
    ctx["cache_control"] = marks_to_json(marks);
    ctx["expect"] = expect;
    ro["revlm_context"] = std::move(ctx);
    ro["messages"] = std::move(tail);
    inc_wire_ = boost::json::serialize(request);
    pending_save_ = static_cast<long long>(full_wire_.size()) - static_cast<long long>(inc_wire_.size());
    stats_.incrementals++;
    return inc_wire_;
}

bool ContextSession::is_conflict(long status, const std::string &error_body)
{
    if (status != 409) return false;
    const nlohmann::json j = nlohmann::json::parse(error_body, nullptr, false);
    if (!j.is_object()) return false;
    const auto err = j.find("error");
    if (err == j.end() || !err->is_object()) return false;
    const auto t = err->find("type");
    if (t == err->end() || !t->is_string()) return false;
    const std::string type = *t;
    return type == "revlm_context_miss" || type == "revlm_context_mismatch";
}

std::string ContextSession::resend_full()
{
    stats_.misses++;
    inc_wire_.clear(); // 这一轮的增量作废：命中账不再落
    pending_save_ = 0;
    return full_wire_;
}

void ContextSession::on_response(const std::string &header_value)
{
    if (!inc_wire_.empty())
    {
        stats_.hits++;
        stats_.bytes_saved += pending_save_;
    }
    inc_wire_.clear();
    pending_save_ = 0;

    // X-Revlm-Context: prefix=<hex>; count=<n>; shape=<hex>
    std::string prefix;
    std::size_t count = 0;
    for (std::size_t pos = 0; pos < header_value.size();)
    {
        const std::size_t semi = header_value.find(';', pos);
        const std::size_t end = semi == std::string::npos ? header_value.size() : semi;
        std::string piece = header_value.substr(pos, end - pos);
        pos = end + 1;
        const std::size_t eq = piece.find('=');
        if (eq == std::string::npos) continue;
        // 头字段以 "; " 分隔，name/value 两头都可能带空格
        const auto trim = [](std::string s) {
            while (!s.empty() && s.front() == ' ') s.erase(0, 1);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            return s;
        };
        const std::string name = trim(piece.substr(0, eq)), value = trim(piece.substr(eq + 1));
        if (name == "prefix")
            prefix = value;
        else if (name == "count")
            std::from_chars(value.data(), value.data() + value.size(), count);
    }
    // 与本地不同只在服务端重组不一致时出现：以本地为准（不覆盖），下一轮 miss 自愈
    if (prefix.size() == 64 && count < nodes_.size() && rllm::to_hex(nodes_[count]) != prefix)
        fprintf(stderr, "[llm] revlm 上下文 prefix 与本地不一致（count=%zu），以本地为准\n",
                count);
}

void ContextSession::record_assistant(const nlohmann::json &message)
{
    // 与下一轮请求同一份转换（upstream/anthropic_messages.cpp）：node 的字节必须跟
    // build_request 产出的完全一致，否则每轮都 miss。
    const nlohmann::json proto{
        {"role", "assistant"}, {"content", anthropic_content_blocks(message)}};
    boost::json::value v = boost::json::parse(proto.dump());
    rllm::strip_cache_control(v); // assistant 侧不带；一条规则管所有消息
    blobs_.push_back(rllm::canonical_message(v));
    nodes_.push_back(rllm::chain_node(nodes_.back(), rllm::blob_hash(v)));
}

} // namespace realagent
