#include "agent/approval.hpp"

#include <chrono>
#include <vector>

#include "json.hpp"

namespace realagent {

ApprovalCoordinator::~ApprovalCoordinator() { cancel_all(); }

Verdict ApprovalCoordinator::await(int agent_id, const std::string &tool_name,
                                   const std::string &params, const EmitFn &emit)
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
    // 客户端不管在看哪段对话都要弹；是谁在问由 emit 盖章（session_id / root）
    nlohmann::json ev;
    ev["id"] = p->id;
    ev["tool"] = tool_name;
    if (nlohmann::json args = nlohmann::json::parse(params, nullptr, false); !args.is_discarded())
        ev["params"] = std::move(args);
    if (emit) emit("permission_request", ev.dump());

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
