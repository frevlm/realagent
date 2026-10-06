#include "agent/approval.hpp"

#include <chrono>
#include <vector>

#include "json.hpp"

namespace realagent {

ApprovalCoordinator::~ApprovalCoordinator() { cancel_all(); }

Verdict ApprovalCoordinator::await(int agent_id, const std::string &tool_name,
                                   const std::string &params)
{
    std::shared_ptr<PendingApproval> p;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        p = std::make_shared<PendingApproval>();
        p->id = "appr_" + std::to_string(next_id_++);
        p->agent_id = agent_id;
        p->tool_name = tool_name;
        p->params = params;
        pending_[p->id] = p;
    }
    // 审批是全局的：客户端不管在看哪个 agent 都要弹，帧里的 agent_id 说明是谁在问
    nlohmann::json ev;
    ev["id"] = p->id;
    ev["agent_id"] = agent_id;
    ev["tool"] = tool_name;
    if (nlohmann::json args = nlohmann::json::parse(params, nullptr, false); !args.is_discarded())
        ev["params"] = std::move(args);
    if (emit_) emit_("permission_request", ev.dump());

    // 阻塞等待裁决（30s 超时按 deny）
    std::unique_lock<std::mutex> lk(p->mtx);
    const bool ok = p->cv.wait_for(lk, std::chrono::seconds(30), [&] { return p->responded; });
    {
        std::lock_guard<std::mutex> pm(mtx_);
        pending_.erase(p->id);
    }
    return ok ? p->verdict : Verdict::Deny;
}

void ApprovalCoordinator::respond(const std::string &id, bool allow)
{
    std::shared_ptr<PendingApproval> p;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        p = it->second;
    }
    std::lock_guard<std::mutex> lk(p->mtx);
    p->verdict = allow ? Verdict::Allow : Verdict::Deny;
    p->responded = true;
    p->cv.notify_one();
}

/* 按 deny 唤醒并从表里摘掉。摘下来再唤醒：拿着 pending_ 的锁去碰每条的 mtx，
 * 就是在一把锁里等另一把。 */
static void release(std::vector<std::shared_ptr<PendingApproval>> &all)
{
    for (auto &p : all)
    {
        std::lock_guard<std::mutex> lk(p->mtx);
        p->verdict = Verdict::Deny;
        p->responded = true;
        p->cv.notify_all();
    }
}

void ApprovalCoordinator::cancel(int agent_id)
{
    std::vector<std::shared_ptr<PendingApproval>> mine;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = pending_.begin(); it != pending_.end();)
        {
            if (it->second->agent_id != agent_id)
            {
                ++it;
                continue;
            }
            mine.push_back(it->second);
            it = pending_.erase(it);
        }
    }
    release(mine);
}

void ApprovalCoordinator::cancel_all()
{
    std::vector<std::shared_ptr<PendingApproval>> all;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto &[_, p] : pending_) all.push_back(p);
        pending_.clear();
    }
    release(all);
}

} // namespace realagent
