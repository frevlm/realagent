/*
 * approval.hpp — 审批：危险工具裁决为 ask 时，agent 线程发 permission_request 并阻塞，
 * 事件循环线程收到 POST /approval-response 后唤醒它。30 秒没裁决按 deny。
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace realagent {

/* Ask 不是第三种结论，是"得问人"；问完只有放行与拒绝。 */
enum class Verdict { Allow,
                     Deny,
                     Ask };

struct PendingApproval {
    std::string id;
    int agent_id = 0; // 谁在问：中断 A 不许掐掉 B 的审批
    std::string tool_name;
    std::string params;
    Verdict verdict = Verdict::Deny;
    bool responded = false;
    std::mutex mtx;
    std::condition_variable cv;
};

class ApprovalCoordinator {
  public:
    ApprovalCoordinator() = default;
    ~ApprovalCoordinator();

    void set_emit(std::function<void(const std::string &type, const std::string &payload)> emit)
    {
        emit_ = std::move(emit);
    }

    /* 有没有客户端能裁决；没有就别问。 */
    void set_online(std::function<bool()> fn) { online_ = std::move(fn); }
    bool online() const { return !online_ || online_(); }

    /* agent 线程：发 permission_request，阻塞到裁决或 30 秒超时（deny）。 */
    Verdict await(int agent_id, const std::string &tool_name, const std::string &params);

    /* 事件循环线程：收到裁决。 */
    void respond(const std::string &id, bool allow);

    /* 按 deny 唤醒某个 agent 挂着的全部审批。 */
    void cancel(int agent_id);

  private:
    void cancel_all();

    std::function<bool()> online_;
    std::function<void(const std::string &, const std::string &)> emit_;
    std::mutex mtx_;
    std::unordered_map<std::string, std::shared_ptr<PendingApproval>> pending_;
    uint64_t next_id_ = 1;
};

} // namespace realagent
