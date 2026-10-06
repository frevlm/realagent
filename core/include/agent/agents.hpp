/*
 * agents.hpp — 活着的 agent 与它们之间的有向边（ADR-0019）
 *
 * `A → B`：A 知道 B、能往 B 的收件箱投消息。边只在创建时由派生方定下，
 * agent 没有「列出所有 agent」的能力——看不见就是没有权限。
 * 完成通知逆着边回流：B 跑完，通知所有指向 B 的 agent。
 */
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/agent.hpp"

namespace realagent {

class Agents {
  public:
    static constexpr int MAX_SIZE = 1000;

    Agents(CoreContext &ctx, ApprovalCoordinator &approval) : ctx_(ctx), approval_(approval) {}
    ~Agents();

    /* by <= 0 表示客户端建的。in / out 是新 agent 的入边与出边。
     * def_body 由派生方解析好再传。成功返回 id (>0)，失败返回 0 并写 err。 */
    int create(const std::string &workdir, int by, const std::vector<int> &in,
               const std::vector<int> &out, std::string &err, const std::string &def_body = {});

    bool post(int to, const std::string &text);

    /* id 跑完了：沿入边通知邻居。由那个 agent 自己的线程调用。 */
    void on_done(int id, const std::string &summary);

    void close(int id);

    nlohmann::json list() const;
    Agent *find(int id);

    /* session_id → 打开着它的 agent_id */
    std::map<std::string, int> openers() const;

  private:
    CoreContext &ctx_;
    ApprovalCoordinator &approval_;

    mutable std::mutex mtx_;
    int cnt_ = 0;
    std::unique_ptr<Agent> nodes_[MAX_SIZE];
    std::vector<int> edges_[MAX_SIZE];
};

} // namespace realagent
