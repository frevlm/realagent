#include "agent/agents.hpp"

namespace realagent {

namespace {

/* 析构在锁外：析构要 join 那条线程，而它收工时会回来拿图锁通知邻居。
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

bool Agents::has_edge(int from, int to) const
{
    const auto it = nodes_.find(from);
    return it != nodes_.end() && it->second.out.contains(to);
}

/* 收方有指向发方的边就署名，没有就跟人发的一模一样（ADR-0019 §5）。 */
void Agents::deliver(int from, int to, const std::string &text)
{
    const std::string tag = has_edge(to, from) ? "[from " + std::to_string(from) + "] " : "";
    nodes_.at(to).agent->post(tag + text);
}

int Agents::add(const std::string &group, const std::string &workdir, const std::string &root,
                const std::string &home, const std::string &def_body, const std::string &session_id,
                std::string &err)
{
    if (workdir.empty())
    {
        err = "workdir is required";
        return 0;
    }
    if (nodes_.size() >= MAX_LIVE)
    {
        err = "too many live agents";
        return 0;
    }
    const int id = ++last_id_;
    const std::string dir = home.empty() ? std::string() : home + "/sub";
    auto a = std::make_shared<Agent>(ctx_of_(group), approval_, workdir, id, this, dir, root,
                                     def_body, session_id);
    nodes_[id] = Node{.group = group,
                      .root = a->root(),
                      .home = home.empty() ? a->session_dir() : home,
                      .agent = std::move(a)};
    return id;
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
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
    {
        if (n.agent->session_id() != session_id) continue;
        if (n.group == client && n.root == session_id) return n.agent;
        // 两个 agent 往同一个文件里追加，那段对话就毁了
        err = "这段对话在另一个窗口里开着";
        return nullptr;
    }
    const int id = add(client, workdir, {}, {}, {}, session_id, err);
    return id > 0 ? nodes_.at(id).agent : nullptr;
}

int Agents::spawn(int by, const std::string &workdir, const std::vector<int> &in,
                  const std::vector<int> &out, std::string &err, const std::string &def_body,
                  const std::string &prompt)
{
    err.clear();
    std::lock_guard<std::mutex> lk(mtx_);
    const auto parent = nodes_.find(by);
    if (parent == nodes_.end())
    {
        err = "no such agent: " + std::to_string(by);
        return 0;
    }
    // 不能授出自己没有的能力
    for (const auto *ids : {&in, &out})
        for (int x : *ids)
            if (x != by && !has_edge(by, x))
            {
                err = "cannot grant an edge to agent " + std::to_string(x) + ": you have no edge to it";
                return 0;
            }

    const Node &p = parent->second;
    const int id = add(p.group, workdir, p.root, p.home, def_body, {}, err);
    if (id <= 0) return 0;
    for (int x : in) nodes_.at(x).out.insert(id);
    for (int x : out) nodes_.at(id).out.insert(x);
    if (!prompt.empty()) deliver(by, id, prompt);
    return id;
}

bool Agents::send(int from, int to, const std::string &text)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!has_edge(from, to)) return false;
    deliver(from, to, text);
    return true;
}

void Agents::on_done(int id, const std::string &summary)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto &[from, n] : nodes_)
        if (n.out.contains(id)) n.agent->post("[" + std::to_string(id) + " done] " + summary, /*notice=*/true);
}

std::shared_ptr<Agent> Agents::detach(int id)
{
    const auto it = nodes_.find(id);
    if (it == nodes_.end()) return nullptr;
    std::shared_ptr<Agent> a = std::move(it->second.agent);
    nodes_.erase(it);
    // 边的语义是「我知道它存在」：它不在了，指向它的边就是假的
    for (auto &[_, n] : nodes_) n.out.erase(id);
    return a;
}

void Agents::close_group(const std::string &client)
{
    std::vector<std::shared_ptr<Agent>> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<int> ids;
        for (const auto &[id, n] : nodes_)
            if (n.group == client) ids.push_back(id);
        for (int id : ids) doomed.push_back(detach(id));
    }
    retire(std::move(doomed));
}

void Agents::interrupt(const std::string &client, const std::string &session_id)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
        if (n.group == client && n.root == session_id) n.agent->interrupt();
}

std::string Agents::state(const std::string &client, const std::string &session_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
        if (n.root == session_id && n.agent->session_id() == session_id)
            return n.group != client ? "elsewhere" : n.agent->running() ? "running"
                                                                        : "";
    return "";
}

/* 不区分「不存在」与「不是你那一组的」：区分了就等于告诉调用方别的组里有什么。 */
std::shared_ptr<Agent> Agents::find(const std::string &client, const std::string &session_id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto &[_, n] : nodes_)
        if (n.group == client && n.root == session_id && n.agent->session_id() == session_id)
            return n.agent;
    return nullptr;
}

std::shared_ptr<Agent> Agents::node(int id) const
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.agent;
}

} // namespace realagent
