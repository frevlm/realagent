/*
 * agents.hpp — 打开着的对话，以及对话派生子 agent 的那一处（ADR-0021 / ADR-0029 / ADR-0033）
 *
 * 客户端用 session_id 指一段对话，agent 是 core 内部的事。每段对话一个 agent，
 * 带一个创建时定下的 group：属于哪个客户端，跨组一律当不存在。
 *
 * 子 agent 不在这张表里：它是 spawn 那次工具调用的局部变量，跑完就析构（ADR-0033）。
 */
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agent/agent.hpp"

namespace realagent {

/* 给一组 agent 用的上下文：事件只推给这一组的客户端，审批只问这一组的客户端。 */
using ContextOf = std::function<CoreContext(const std::string &client)>;

class Agents {
  public:
    /* 同时打开着的对话上限。id 不复用：审批按 id 认人，旧 id 不能指到新 agent 上。 */
    static constexpr size_t MAX_LIVE = 1000;

    Agents(ContextOf ctx_of, ApprovalCoordinator &approval)
        : ctx_of_(std::move(ctx_of)), approval_(approval) {}
    ~Agents();

    /* 客户端的一段对话：打开着就是它，没打开就建一个 agent 打开它（盘上没有就是新的）。
     * session_id 由客户端给；第一条消息到了才调这里，所以没有从没说过话的 agent。
     * 失败返回空并写 err。 */
    std::shared_ptr<Agent> open(const std::string &client, const std::string &workdir,
                                const std::string &session_id, std::string &err);

    /* 对话 by 派生一个子 agent：同组、同一段对话、落在对话那一头的 sub/。
     * 它自己不能再派生。每跑完一趟调一次 on_done。
     * def_body 接在它 system prompt 末尾。by 不是打开着的对话就返回空并写 err。 */
    std::shared_ptr<Agent> spawn(int by, const std::string &workdir, const std::string &def_body,
                                 OnDone on_done,
                                 std::string &err);

    /* 停下一段对话：子 agent 由那次 spawn 调用跟着停（ADR-0033）。 */
    void interrupt(const std::string &client, const std::string &session_id);

    /* 关掉一组：客户端退出，或断线满 60 秒。 */
    void close_group(const std::string &client);

    /* 客户端那一组里打开着这段对话的 agent；没有就空。 */
    std::shared_ptr<Agent> find(const std::string &client, const std::string &session_id) const;

    /* 对话的状态：running = client 那一组正在跑，elsewhere = 在别的组里开着，空 = 都不是。 */
    std::string state(const std::string &client, const std::string &session_id) const;

  private:
    struct Node {
        std::string group;
        std::shared_ptr<Agent> agent;
    };

    ContextOf ctx_of_;
    ApprovalCoordinator &approval_;

    mutable std::mutex mtx_;
    int last_id_ = 0;
    std::map<int, Node> nodes_;
};

} // namespace realagent
