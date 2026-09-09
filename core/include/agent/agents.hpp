/*
 * agents.hpp — core 里活着的那些 Agent，以及它们之间的边（ADR-0019）
 *
 * 边是一张表：`A → B` 表示 A 知道 B 存在、能往 B 的收件箱投消息。有向，双向就是两条。
 * **只有一种边，边上不带类型**——「A 创建了 B」与「A 想给 C 发消息」是同一条边。
 *
 * 没有边就不知道对方存在：这里不给 agent 提供任何「列出所有 agent」的能力，
 * 于是边只能在创建对方时定下，不能靠查询产生。这是能力模型，不是访问控制——
 * 不存在「有权限但看不见」这种状态。
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

    /* 建一个 agent。workdir 必传、无默认（ADR-0019）。
     * by <= 0 为人（客户端）建的，人不是图上的节点。
     * in / out 是新 agent 的入边与出边（数字 id）：派生方决定它被谁知道、能找谁。
     * 成功返回新 agent 的数字 id (>0)，失败返回 0 并写 err。 */
    /* persona 是 agent 定义的正文（ADR-0024 §8），接在新 agent 的 system prompt 后面。
     * 空 = 没指定，system prompt 与加这个功能之前一个字不差。
     * **由派生方解析好再传**：它在自己的 system prompt 里看见了哪些名字，就该拿到哪一份正文。 */
    int create(const std::string &workdir, int by,
               const std::vector<int> &in,
               const std::vector<int> &out, std::string &err,
               const std::string &persona = {});

    /* 投一条消息进目标 agent 的收件箱。没有 to 这个 agent 返回 false。 */
    bool post(int to, const std::string &text);

    /* 某个 agent 跑完了：沿入边把完成通知投给关心它的邻居。
     * 边指向你关心的那个 agent，所以消息顺着边走、完成通知逆着边回来。
     * 由 agent 自己的线程调用。 */
    void on_done(int id, const std::string &summary);

    void interrupt(int id);
    void close(int id);

    /* 清单（GET /agents） */
    nlohmann::json list() const;

    /* 存在即返回指针，否则 nullptr */
    Agent *find(int id);

    /* 「哪个会话被哪个 agent 打开着」：session_id → agent_id */
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
