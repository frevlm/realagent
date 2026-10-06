/*
 * hub.cpp — 读配置、起连接、拼工具表。协议在 client.cpp。
 */
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <thread>

#include "mcp/mcp.hpp"
#include "plugin.hpp"

namespace realagent {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

/* 端点认的工具名字符是 [a-zA-Z0-9_-]。只换字符、不截断：名字要永久写进会话记录。 */
std::string sanitize_tool_name(std::string s)
{
    std::replace_if(
        s.begin(), s.end(),
        [](char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'); },
        '_');
    return s;
}

std::string subst(std::string s, std::string_view from, const std::string &to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

/* http 条目 → 桥接进程的 argv。模板里 `{url}` 换成 url；子数组是「每个 header 一组」，
 * 没有 header 时整组不出现。 */
json bridge_argv(const json &e, const json &bridge)
{
    if (!bridge.is_array() || bridge.empty())
        throw std::runtime_error("type=\"http\" 要一个桥接进程，而配置里的 mcp_http_bridge "
                                 "是空的——填一条命令模板，或者把这个 server 改成 stdio");
    const auto url = e.at("url").get<std::string>();
    if (url.empty()) throw std::runtime_error("url 是空串");

    json argv = json::array();
    for (const auto &one : bridge)
    {
        if (!one.is_array())
        {
            argv.push_back(subst(one.get<std::string>(), "{url}", url));
            continue;
        }
        for (const auto &[hk, hv] : e.value("headers", json::object()).items())
            for (const auto &part : one)
                argv.push_back(subst(subst(part.get<std::string>(), "{name}", hk), "{value}",
                                     hv.get<std::string>()));
    }
    return argv;
}

/* 一个条目 → 归一过的规格 {name, command, args, env}；未知键在这里落下，否则同一个 server
 * 会因为一个被忽略的键起两个进程。返回 nullopt 且 err 为空 = 用户明确关掉的。
 * 类型错由 json 库抛（自带键名），业务错自己抛，同一个 catch 接。 */
std::optional<json> parse_entry(const PluginRoot &r, const std::string &name, const json &e,
                                const json &bridge, std::string &err)
{
    /* 只展开 ${CLAUDE_PLUGIN_ROOT}：它有唯一答案（plugin 装在哪）。
     * ${workspaceFolder} 一类对共享连接有 N 个答案，拒绝。 */
    const auto str = [&r](const json &v) {
        auto s = v.get<std::string>();
        if (const auto at = s.find(kPluginRootVar); at != std::string::npos)
        {
            if (r.root.empty())
                throw std::runtime_error("`" + s + "` 里有 " + kPluginRootVar +
                                         "——它只在 plugin 里有意义，这份配置是你自己写的，"
                                         "请写绝对路径");
            s.replace(at, std::string(kPluginRootVar).size(), r.root.string());
        }
        if (s.find("${") != std::string::npos)
            throw std::runtime_error("`" + s +
                                     "` 里有 ${...}——那是别家客户端的变量，本项目不展开"
                                     "（连接是进程级的，不属于任何目录）。那个参数是这个 server "
                                     "被允许触碰的范围，请改成绝对路径：写死它就是明确授一次权");
        return s;
    };
    try
    {
        if (!e.value("enabled", true)) return std::nullopt;

        const auto type = e.value("type", std::string("stdio"));
        const bool http = type == "http" || type == "sse";
        if (!http && type != "stdio")
            throw std::runtime_error("type=\"" + type + "\" 不认识（只有 stdio 与 http）");

        json argv = json::array(), env = json::object();
        if (http)
            argv = bridge_argv(e, bridge);
        else
        {
            argv.push_back(str(e.at("command")));
            if (argv[0] == "") throw std::runtime_error("command 是空串");
            for (const auto &one : e.value("args", json::array())) argv.push_back(str(one));
        }
        for (const auto &[k, v] : e.value("env", json::object()).items())
            env[k] = http ? v.get<std::string>() : str(v);

        return json{{"name", name},
                    {"command", argv[0]},
                    {"args", json(argv.begin() + 1, argv.end())},
                    {"env", std::move(env)}};
    } catch (const std::exception &ex)
    {
        err = name + ": " + ex.what();
        return std::nullopt;
    }
}

/* 读一份 mcp.json 合进 out。文件不存在什么都不做。 */
void read_file(const PluginRoot &r, const json &bridge, std::map<std::string, json> &out,
               std::vector<std::string> &errors)
{
    const fs::path &path = r.mcp_file;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    json servers;
    try
    {
        std::ifstream f(path);
        servers = json::parse(f).value("mcpServers", json::object());
    } catch (const json::exception &ex)
    {
        errors.push_back(path.string() + ": " + ex.what());
        return;
    }
    for (const auto &[name, entry] : servers.items())
    {
        // 前缀只在这里加：两个 plugin 各带一个叫 fs 的 server 是两条，不是覆盖
        const std::string key = r.prefix().empty() ? name : r.prefix() + "_" + name;
        std::string err;
        if (auto c = parse_entry(r, key, entry, bridge, err))
            out[key] = std::move(*c);
        else
        {
            out.erase(key); // 近的那份关掉或坏了，远的也不该顶上
            if (!err.empty()) errors.push_back(path.string() + ": " + err);
        }
    }
}

} // namespace

McpConfig load_mcp_config(const std::string &workdir, const json &bridge)
{
    McpConfig out;
    std::map<std::string, json> merged;
    for (const PluginRoot &r : plugin_roots(workdir)) read_file(r, bridge, merged, out.errors);
    for (auto &[name, c] : merged) out.servers.push_back(std::move(c));
    return out;
}

McpHub::Lease McpHub::open(const std::string &workdir, const json &bridge)
{
    McpConfig cfg = load_mcp_config(workdir, bridge);
    Lease lease;
    lease.errors = std::move(cfg.errors);

    // 名字与规格分家：规格（去掉 name）dump 出来就是连接的键
    std::vector<std::string> names, keys;
    std::map<std::string, json> spec_of;      // 键 → 规格
    std::map<std::string, std::string> label; // 键 → 报错时冠的名（同一进程的所有名字）
    for (json c : cfg.servers)
    {
        names.push_back(c.at("name").get<std::string>());
        c.erase("name");
        keys.push_back(c.dump());
        auto [it, fresh] = label.emplace(keys.back(), names.back());
        if (!fresh) it->second += " / " + names.back();
        spec_of.emplace(keys.back(), std::move(c));
    }

    std::map<std::string, std::shared_ptr<McpClient>> have;
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto &[k, _] : spec_of)
        {
            const auto it = conns_.find(k);
            if (auto p = it == conns_.end() ? nullptr : it->second.lock())
                have.emplace(k, std::move(p));
            else
                todo.push_back(k);
        }
    }

    // 没连的并行连：npx 第一次要下包，一个慢的不该拖住别的
    std::vector<std::shared_ptr<McpClient>> fresh(todo.size());
    std::vector<std::string> errs(todo.size());
    {
        std::vector<std::thread> ts;
        for (size_t i = 0; i < todo.size(); ++i)
            ts.emplace_back([&, i] { fresh[i] = McpClient::start(spec_of[todo[i]], errs[i]); });
        for (auto &t : ts) t.join();
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < todo.size(); ++i)
        {
            if (!fresh[i])
            {
                lease.errors.push_back(label[todo[i]] + ": " + errs[i]);
                continue;
            }
            conns_[todo[i]] = fresh[i];
            have.emplace(todo[i], fresh[i]);
        }
    }

    for (size_t i = 0; i < keys.size(); ++i)
        if (const auto it = have.find(keys[i]); it != have.end())
            lease.conns.push_back({names[i], it->second});

    // 从 server 的工具对象里只取 name / description / inputSchema；一律 dangerous
    // （规范自己说 annotations 不可信）
    for (const Lease::Conn &c : lease.conns)
        for (const auto &t : c.client->tools())
        {
            if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) continue;
            const std::string remote = t["name"].get<std::string>();
            const auto schema = t.find("inputSchema");
            lease.tools.push_back(json{
                {"name", sanitize_tool_name(c.name + "__" + remote)},
                {"description", t.value("description", std::string())},
                {"input_schema",
                 schema != t.end() && schema->is_object() ? *schema : json{{"type", "object"}}},
                {"_core",
                 {{"label", remote}, {"dangerous", true}, {"server", c.name}, {"remote_name", remote}}}});
        }
    return lease;
}

} // namespace realagent
