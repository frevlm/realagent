#include "agent/agents.hpp"

#include <algorithm>

namespace realagent {

Agents::~Agents()
{
    std::vector<std::unique_ptr<Agent>> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int i = 1; i <= cnt_; ++i)
        {
            if (nodes_[i]) doomed.push_back(std::move(nodes_[i]));
            edges_[i].clear();
        }
    }
    doomed.clear();
}

int Agents::create(const std::string &workdir, int by,
                   const std::vector<int> &in,
                   const std::vector<int> &out, std::string &err,
                   const std::string &def_body)
{
    err.clear();
    if (workdir.empty())
    {
        err = "workdir is required";
        return 0;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    if (cnt_ + 1 >= MAX_SIZE)
    {
        err = "agent pool is full";
        return 0;
    }

    const int id = ++cnt_;
    nodes_[id] = std::make_unique<Agent>(ctx_, approval_, workdir, id, this, by > 0, def_body);
    for (int x : in)
    {
        if (x > 0 && x < MAX_SIZE) edges_[x].push_back(id);
    }
    for (int x : out)
    {
        if (x > 0 && x < MAX_SIZE) edges_[id].push_back(x);
    }
    return id;
}

bool Agents::post(int to, const std::string &text)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (to <= 0 || to >= MAX_SIZE || !nodes_[to]) return false;
    nodes_[to]->post(text);
    return true;
}

void Agents::on_done(int id, const std::string &summary)
{
    std::lock_guard<std::mutex> lk(mtx_);
    for (int from = 1; from <= cnt_; ++from)
    {
        if (!nodes_[from]) continue;
        for (int to : edges_[from])
        {
            if (to == id)
            {
                nodes_[from]->post("[" + std::to_string(id) + " done] " + summary);
                break;
            }
        }
    }
}

void Agents::interrupt(int id)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (id > 0 && id <= cnt_ && nodes_[id])
    {
        nodes_[id]->interrupt();
    }
}

void Agents::close(int id)
{
    std::unique_ptr<Agent> doomed;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (id <= 0 || id >= MAX_SIZE || !nodes_[id]) return;
        nodes_[id]->interrupt();
        doomed = std::move(nodes_[id]);
        edges_[id].clear();
        for (int from = 1; from <= cnt_; ++from)
        {
            auto &v = edges_[from];
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
        }
    }
    doomed.reset();
}

nlohmann::json Agents::list() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 1; i <= cnt_; ++i)
    {
        const auto &a = nodes_[i];
        if (!a) continue;
        nlohmann::json e{{"id", i},
                         {"workdir", a->workdir()},
                         {"state", a->running() ? "running" : "idle"},
                         {"session_id", a->session_id()}};
        nlohmann::json outs = nlohmann::json::array(), ins = nlohmann::json::array();
        for (int to : edges_[i]) outs.push_back(to);
        for (int from = 1; from <= cnt_; ++from)
        {
            if (!nodes_[from]) continue;
            for (int to : edges_[from])
            {
                if (to == i)
                {
                    ins.push_back(from);
                    break;
                }
            }
        }
        e["out_edges"] = std::move(outs);
        e["in_edges"] = std::move(ins);
        arr.push_back(std::move(e));
    }
    return arr;
}

std::map<std::string, int> Agents::openers() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    std::map<std::string, int> out;
    for (int i = 1; i <= cnt_; ++i)
    {
        const auto &a = nodes_[i];
        if (a)
        {
            out[a->session_id()] = i;
        }
    }
    return out;
}

Agent *Agents::find(int id)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (id <= 0 || id >= MAX_SIZE || !nodes_[id]) return nullptr;
    return nodes_[id].get();
}

} // namespace realagent
