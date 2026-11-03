#include "agent/agents.hpp"

namespace realagent {

namespace {

/* 析构在锁外：析构要 join 那条线程，而线程里可能正有人来问 state()。
 * 先全部 interrupt 再逐个析构，几个 agent 并行地停，不是排队停。 */
void retire(std::vector<std::shared_ptr<Agent>> doomed)
{
    for (auto &a : doomed) a->interrupt();
    doomed.clear();
}

} // namespace

Agents::~Agents()
{
    std::vector<std::shared_ptr<Agent>> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto &[_, n] : nodes_) doomed.push_back(std::move(n.agent));
        nodes_.clear();
    }
    retire(std::move(doomed));
}

std::shared_ptr<Agent> Agents::open(const std::string &client, const std::string &workdir,
                                    const std::string &session_id, std::string &err)
{
    err.clear();
    if (session_id.empty() || session_id.find('/') != std::string::npos)
    {
        err = "session_id is required and must not contain '/'";
        return nullptr;
    }
    if (workdir.empty())
    {
        err = "workdir is required";
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
    {
        if (n.agent->session_id() != session_id) continue;
        if (n.group == client) return n.agent;
        // 两个 agent 往同一个文件里追加，那段对话就毁了
        err = "这段对话在另一个窗口里开着";
        return nullptr;
    }
    if (nodes_.size() >= MAX_LIVE)
    {
        err = "too many live agents";
        return nullptr;
    }
    const int id = ++last_id_;
    auto a = std::make_shared<Agent>(ctx_of_(client), approval_, workdir, id, this, std::string(),
                                     std::string(), std::string(), session_id);
    nodes_[id] = Node{.group = client, .agent = a};
    return a;
}

std::shared_ptr<Agent> Agents::spawn(int by, const std::string &workdir, const std::string &def_body,
                                     OnDone on_done,
                                     std::string &err)
{
    err.clear();
    if (workdir.empty())
    {
        err = "workdir is required";
        return nullptr;
    }
    std::string group, home, root;
    int id = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = nodes_.find(by);
        if (it == nodes_.end())
        {
            err = "no such agent: " + std::to_string(by);
            return nullptr;
        }
        group = it->second.group;
        home = it->second.agent->session_dir();
        root = it->second.agent->session_id();
        id = ++last_id_;
    }
    // 锁外建：建 agent 要连 MCP、跑 SessionStart hook，不能让别的对话干等
    return std::make_shared<Agent>(ctx_of_(group), approval_, workdir, id, nullptr, home + "/sub",
                                   root, def_body, std::string(), std::move(on_done));
}

void Agents::close_group(const std::string &client)
{
    std::vector<std::shared_ptr<Agent>> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = nodes_.begin(); it != nodes_.end();)
        {
            if (it->second.group != client)
            {
                ++it;
                continue;
            }
            doomed.push_back(std::move(it->second.agent));
            it = nodes_.erase(it);
        }
    }
    retire(std::move(doomed));
}

void Agents::interrupt(const std::string &client, const std::string &session_id)
{
    if (const auto a = find(client, session_id)) a->interrupt();
}

std::string Agents::state(const std::string &client, const std::string &session_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
    {
        if (n.agent->session_id() != session_id) continue;
        if (n.group != client) return "elsewhere";
        // 子 agent 跑着时对话这一头正卡在 spawn 里，所以只问它就够（ADR-0033）
        return n.agent->running() ? "running" : "";
    }
    return {};
}

/* 不区分「不存在」与「不是你那一组的」：区分了就等于告诉调用方别的组里有什么。 */
std::shared_ptr<Agent> Agents::find(const std::string &client, const std::string &session_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
        if (n.group == client && n.agent->session_id() == session_id) return n.agent;
    return nullptr;
}

} // namespace realagent
