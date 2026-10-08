/*
 * agents.hpp — 活着的 agent、它们之间的有向边、它们属于哪一组（ADR-0019 / ADR-0021 / ADR-0029）
 *
 * 两层地址。图里用 agent id：`A → B` 表示 A 知道 B、能往 B 的收件箱投消息，边只在派生时定下，
 * 完成通知逆着边回流。客户端用 session_id：它看见的只有对话，agent 是 core 内部的事。
 *
 * 每个节点带两个创建时定下、之后不变的标签，都从创建者那里继承：
 * - group：属于哪个客户端。跨组一律当不存在。
 * - root：属于用户看见的哪段对话。只管显示与中断，投递只看边。
 */
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "agent/agent.hpp"

namespace realagent {

/* 给一组 agent 用的上下文：事件只推给这一组的客户端，审批只问这一组的客户端。 */
using ContextOf = std::function<CoreContext(const std::string &client)>;

class Agents {
  public:
    /* 同时活着的上限。id 不复用（模型历史里的旧 id 不能指到新 agent 上），所以限的是活着的数。 */
    static constexpr size_t MAX_LIVE = 1000;

    Agents(ContextOf ctx_of, ApprovalCoordinator &approval)
        : ctx_of_(std::move(ctx_of)), approval_(approval) {}
    ~Agents();

    /* 客户端的一段对话：打开着就是它，没打开就建一个 agent 打开它（盘上没有就是新的）。
     * session_id 由客户端给；第一条消息到了才调这里，所以没有从没说过话的 agent。
     * 失败返回空并写 err。 */
    std::shared_ptr<Agent> open(const std::string &client, const std::string &workdir,
                                const std::string &session_id, std::string &err);

    /* by 派生一个 agent。in / out 是新 agent 的入边与出边，必须 ⊆ by 的出边 ∪ {by}——
     * 不能授出自己没有的能力。prompt 非空就作为第一条消息投进去。成功返回 id，失败返回 0。 */
    int spawn(int by, const std::string &workdir, const std::vector<int> &in,
              const std::vector<int> &out, std::string &err, const std::string &def_body = {},
              const std::string &prompt = {});

    /* from 给 to 投一条消息。from 没有到 to 的边，就跟 to 不存在一样回 false。 */
    bool send(int from, int to, const std::string &text);

    /* id 跑完了：沿入边通知邻居。由那个 agent 自己的线程调用。 */
    void on_done(int id, const std::string &summary);

    /* 停下一段对话：它和它派生出去的全部，用户眼里那是一件事。 */
    void interrupt(const std::string &client, const std::string &session_id);

    /* 关掉一组：客户端退出，或断线满 60 秒。 */
    void close_group(const std::string &client);

    /* 客户端那一组里打开着这段对话的 agent；没有就空。 */
    std::shared_ptr<Agent> find(const std::string &client, const std::string &session_id) const;
    /* 图里按 id 找。 */
    std::shared_ptr<Agent> node(int id) const;

    /* 对话的状态：running = client 那一组正在跑，elsewhere = 在别的组里开着，空 = 都不是。 */
    std::string state(const std::string &client, const std::string &session_id) const;

  private:
    struct Node {
        std::string group;
        std::string root;
        std::string home; // 对话的会话目录；派生的落在它下面的 sub/
        std::shared_ptr<Agent> agent;
        std::set<int> out; // 出边
    };

    bool has_edge(int from, int to) const;
    void deliver(int from, int to, const std::string &text);
    /* 建节点，调用方持锁。root / home 为空表示自己就是一段对话的开头。 */
    int add(const std::string &group, const std::string &workdir, const std::string &root,
            const std::string &home, const std::string &def_body, const std::string &session_id,
            std::string &err);
    /* 从图上摘下：删掉它和所有指向它的边。调用方持锁。 */
    std::shared_ptr<Agent> detach(int id);

    ContextOf ctx_of_;
    ApprovalCoordinator &approval_;

    mutable std::mutex mtx_;
    int last_id_ = 0;
    std::map<int, Node> nodes_;
};

} // namespace realagent
