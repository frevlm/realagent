/*
 * hub.cpp — 读配置、起连接、拼工具表
 *
 * 这个文件里没有一行协议。协议在 client.cpp，这里只回答三件事：
 * 有哪些 server、哪些已经连着、模型看见的那张表长什么样。
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

namespace {

/* 工具名里的非法字符换成 `_`（端点认的字符类是 `[a-zA-Z0-9_-]`，考据见 ADR-0023）。
 *
 * **只换字符，不截断。** 两件事的判据不同：长度那一半是用户写的配置键，他改得动，
 * 撞上就让请求爆、端点会指名道姓；字符是 server 起的名字，他一个字都改不了，
 * 而一个这样的 server 能让每一次请求都 400。
 *
 * 替换必须是确定性的：这个名字要永久写进会话记录，不能随算法或长度变。 */
std::string sanitize_tool_name(std::string s)
{
    std::replace_if(
        s.begin(), s.end(),
        [](char c) { return !(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'); },
        '_');
    return s;
}

/* 把 s 里全部 from 换成 to。模板替换只此一处，不长第二份。 */
std::string subst(std::string s, std::string_view from, const std::string &to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

/* 一条 http 规格 → 一条 stdio 规格，靠配置里那个桥接模板（ADR-0024 §5）。
 *
 * 模板里 `{url}` 换成 url；**子数组是「每个 header 重复一次」的那一组**，
 * 一个 header 都没有时整组不出现——一条规则把 header 并进同一个模板，
 * 不为它另开一个配置键。
 *
 * 边界写出来，不靠猜：`--header` 与它后面那个占位符是一组，这件事从参数自身看不出来，
 * 而猜错的后果是往命令行里塞一个孤零零的 `--header`。
 *
 * 抛出去的错由 parse_entry 那个 catch 接住，跟别的失败走同一条路。 */
nlohmann::json bridge_command(const nlohmann::json &e, const nlohmann::json &bridge)
{
    if (!bridge.is_array() || bridge.empty())
        throw std::runtime_error("type=\"http\" 要一个桥接进程，而配置里的 mcp_http_bridge "
                                 "是空的——填一条命令模板，或者把这个 server 改成 stdio");
    const auto url = e.at("url").get<std::string>();
    if (url.empty()) throw std::runtime_error("url 是空串");

    std::vector<std::pair<std::string, std::string>> headers;
    for (const auto &[k, v] : e.value("headers", nlohmann::json::object()).items())
        headers.emplace_back(k, v.get<std::string>());

    nlohmann::json argv = nlohmann::json::array();
    for (const auto &one : bridge)
    {
        if (!one.is_array())
        {
            argv.push_back(subst(one.get<std::string>(), "{url}", url));
            continue;
        }
        // 子数组 = header 那一组：有几个 header 就整组出现几次，零个就一次都不出现
        for (const auto &[hk, hv] : headers)
            for (const auto &part : one)
                argv.push_back(subst(subst(part.get<std::string>(), "{name}", hk), "{value}", hv));
    }
    return argv;
}

/* http 条目 → 归一过的启动规格。走的是与 stdio 完全相同的四个键，
 * 于是**下游拿到的东西一模一样**：连接的键、去重、生死、转发，一律不知道它原来是 http。 */
std::optional<nlohmann::json> bridge_entry(const std::string &name, const nlohmann::json &e,
                                           const nlohmann::json &bridge, std::string &err)
{
    try
    {
        nlohmann::json argv = bridge_command(e, bridge);
        nlohmann::json c{{"name", name},
                         {"command", argv[0]},
                         {"args", nlohmann::json::array()},
                         {"env", nlohmann::json::object()}};
        for (size_t i = 1; i < argv.size(); ++i) c["args"].push_back(argv[i]);
        for (const auto &[k, v] : e.value("env", nlohmann::json::object()).items())
            c["env"][k] = v.get<std::string>();
        return c;
    } catch (const std::exception &ex)
    {
        err = name + ": " + ex.what();
        return std::nullopt;
    }
}

/* 一个条目 → 一条归一过的启动规格（只剩 name / command / args / env 四个键）。
 * 未知键在这里落下：那份对象 dump 出来就是连接的键，留着一个被忽略的 `timeout`，
 * 同一个 server 会因为它起两个进程。
 *
 * **失败只有一个出口**：类型上的错由 json 库抛（`key 'command' not found`、
 * `type must be string, but is number`——自带键名与实际类型，比手写的准），
 * 业务上的错我们自己抛，同一个 catch 接住。err 空 = 用户明确关的，不是错。 */
std::optional<nlohmann::json> parse_entry(const PluginRoot &r, const std::string &name,
                                          const nlohmann::json &e, const nlohmann::json &bridge,
                                          std::string &err)
{
    /* 每个进规格的字符串都过这一道。
     *
     * **只展开 `${CLAUDE_PLUGIN_ROOT}` 一个**（ADR-0024 §4）。判据不是「是不是变量」，
     * 是**这个变量有没有唯一答案**：`${workspaceFolder}` 那一类问的是「当前项目目录」，
     * 而一个进程级共享的连接面对 N 个 agent 就有 N 个答案，展开它就是让访问边界漂移；
     * 这一个问的是「这个 plugin 装在哪」，一个 plugin 一个安装位置，跟谁在调用无关。
     *
     * 隐式 plugin 那儿没有 plugin root，见到它就是错——那份配置是用户自己写的，
     * 他写得出绝对路径。 */
    const auto str = [&r](const nlohmann::json &v) {
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
        if (!e.value("enabled", true)) return std::nullopt; // `{"enabled": false}` = 关掉

        /* `type: "http"` 在这一层就变成一条 stdio 规格（ADR-0024 §5）。
         * **特殊情况在归一里消失，下游一行不动**：没有第二种 transport，没有 curl POST、
         * 没有 SSE 解析、没有 OAuth，读线程与按 id 认领那一套一字不改。 */
        const auto type = e.value("type", std::string("stdio"));
        if (type == "http" || type == "sse") return bridge_entry(name, e, bridge, err);
        if (type != "stdio")
            throw std::runtime_error("type=\"" + type + "\" 不认识（只有 stdio 与 http）");

        const auto command = str(e.at("command"));
        if (command.empty()) throw std::runtime_error("command 是空串");
        nlohmann::json c{{"name", name},
                         {"command", command},
                         {"args", nlohmann::json::array()},
                         {"env", nlohmann::json::object()}};
        for (const auto &one : e.value("args", nlohmann::json::array()))
            c["args"].push_back(str(one));
        for (const auto &[k, v] : e.value("env", nlohmann::json::object()).items())
            c["env"][k] = str(v);
        return c;
    } catch (const std::exception &ex)
    {
        // core 是常驻服务：这里不接住的话，一份手滑的配置能带走所有 agent
        err = name + ": " + ex.what();
        return std::nullopt;
    }
}

/* 读一份文件里的 mcpServers，合进 out。文件不存在 → 什么都不做（多数人一个都没配）。
 * 形状不对 → 记一条错，不拒绝启动（`models.json` 那条先例）。
 *
 * `value("mcpServers", {})` 一句办三件事：缺键当空的、类型不对就抛、拿到就是个对象。 */
void read_file(const PluginRoot &r, const nlohmann::json &bridge,
               std::map<std::string, nlohmann::json> &out, std::vector<std::string> &errors)
{
    const fs::path &path = r.mcp_file;
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    std::ifstream f(path);
    if (!f)
    {
        errors.push_back(path.string() + " 打不开");
        return;
    }
    nlohmann::json servers;
    try
    {
        servers = nlohmann::json::parse(f).value("mcpServers", nlohmann::json::object());
    } catch (const nlohmann::json::exception &ex)
    {
        errors.push_back(path.string() + ": " + ex.what()); // 库的原话带行号列号
        return;
    }
    for (const auto &[name, entry] : servers.items())
    {
        /* **前缀在这一处加，只此一处。** 它同时是表的键和规格里的 name，
         * 也就是工具名 `<前缀>_<键>__<原名>` 的前半截——单下划线连的是「身份」那一半，
         * 双下划线之后才是原名（ADR-0024 §3，照抄 Claude Code 的形状）。
         *
         * 两个 plugin 各带一个键为 `fs` 的 server，于是是两条各自独立的条目，
         * 不是一次覆盖。 */
        const std::string key = r.prefix().empty() ? name : r.prefix() + "_" + name;
        std::string err;
        if (auto c = parse_entry(r, key, entry, bridge, err))
            out[key] = std::move(*c); // 同名整条覆盖：近的那份把远的整条换掉
        else
        {
            // 关掉的和坏掉的动作是同一个：抹掉。近的那份坏了，远的也不该顶上
            out.erase(key);
            if (!err.empty()) errors.push_back(path.string() + ": " + err);
        }
    }
}

} // namespace

McpConfig load_mcp_config(const std::string &workdir, const nlohmann::json &bridge)
{
    McpConfig out;
    std::map<std::string, nlohmann::json> merged;
    // 顺序遍历扫描链：远的在前、近的在后，同名条目就此被近的那份整条换掉（ADR-0024 §1）
    for (const PluginRoot &r : plugin_roots(workdir)) read_file(r, bridge, merged, out.errors);
    // merged 是 std::map，按 key 迭代出来就是按名字有序的：同一份配置两次运行给同一张表
    for (auto &[name, c] : merged) out.servers.push_back(std::move(c));
    return out;
}

McpHub::Lease McpHub::open(const std::string &workdir, const nlohmann::json &bridge)
{
    McpConfig cfg = load_mcp_config(workdir, bridge);
    Lease lease;
    lease.errors = std::move(cfg.errors);

    /* 名字与规格就此分家：规格是「要 exec 什么」，名字是配置给的标签。
     * 于是**规格自己就是连接的键**（ADR-0023 §2 那句话到这儿才字面成立）。 */
    std::vector<std::string> names;
    std::vector<nlohmann::json> specs;
    for (const nlohmann::json &c : cfg.servers)
    {
        names.push_back(c.at("name").get<std::string>());
        nlohmann::json spec = c;
        spec.erase("name");
        specs.push_back(std::move(spec));
    }

    /* 同一份规格只连一次。**去重发生在这一次 open 之内，也发生在池子上**——
     * 两个 plugin 各带一份一模一样的 server 很常见，那是一个进程两个名字。
     * uniq 顺带给每个键留一个人话标签，因为连接自己已经不知道它叫什么了。 */
    std::map<std::string, const nlohmann::json *> uniq; // 键 → 规格
    std::map<std::string, std::string> label;           // 键 → 报错时冠的名
    for (size_t i = 0; i < specs.size(); ++i)
    {
        const std::string k = specs[i].dump();
        uniq.emplace(k, &specs[i]);
        auto [it, fresh] = label.emplace(k, names[i]);
        if (!fresh) it->second += " / " + names[i]; // 同一个进程挂着的所有名字都报出来
    }

    std::map<std::string, std::shared_ptr<McpClient>> have; // 键 → 连接
    std::vector<std::string> todo;
    {
        /* 用 find 不用 operator[]——后者会给每一个没命中的键插一个空 weak_ptr 进去，
         * 那些垃圾还得再擦一遍。 */
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto &[k, spec] : uniq)
        {
            const auto it = conns_.find(k);
            if (auto p = it == conns_.end() ? nullptr : it->second.lock())
                have.emplace(k, std::move(p));
            else
                todo.push_back(k);
        }
    }

    /* 没连的并行连——一个 server 起得慢（`npx` 第一次要下包）不该让别的跟着等。
     * 各线程只写自己那一格，写回池子在 join 之后，所以这里不需要锁。 */
    std::vector<std::shared_ptr<McpClient>> fresh(todo.size());
    std::vector<std::string> errs(todo.size());
    {
        std::vector<std::thread> ts;
        ts.reserve(todo.size());
        for (size_t i = 0; i < todo.size(); ++i)
            ts.emplace_back([&, i] { fresh[i] = McpClient::start(*uniq[todo[i]], errs[i]); });
        for (auto &t : ts) t.join();
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (size_t i = 0; i < todo.size(); ++i)
        {
            if (!fresh[i])
            {
                // 坏 server 跳过，core 照常起（models.json 那条先例，不是 settings.json 那条）
                lease.errors.push_back(label[todo[i]] + ": " + errs[i]);
                continue;
            }
            conns_[todo[i]] = fresh[i];
            have.emplace(todo[i], fresh[i]);
        }
    }

    // 每条配置条目挂到它那个连接上。没连上的那份规格，用它的条目就此不存在
    for (size_t i = 0; i < specs.size(); ++i)
        if (const auto it = have.find(specs[i].dump()); it != have.end())
            lease.conns.push_back({names[i], it->second});

    /* 拼表。core 从 server 手里只取三样，其余自己写——外部遵守协议，内部 core 知道
     * 该写什么。MCP 的 title / annotations / icons / outputSchema / execution 一律不收：
     * 端点的工具定义里没有它们的位置，annotations 更是规范自己说了不可信。 */
    for (const Lease::Conn &c : lease.conns)
    {
        for (const auto &t : c.client->tools())
        {
            if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) continue;
            const std::string remote = t["name"].get<std::string>();
            nlohmann::json one;
            // 名字 `<plugin 前缀>_<配置的键>__<原名>`（前缀已并进 c.name），
            // 非法字符换 `_`、不截断（判据见 sanitize_tool_name）
            one["name"] = sanitize_tool_name(c.name + "__" + remote);
            one["description"] = t.value("description", std::string());
            const auto schema = t.find("inputSchema");
            one["input_schema"] = schema != t.end() && schema->is_object()
                                      ? *schema
                                      : nlohmann::json{{"type", "object"}};
            one["_core"] = {{"label", remote},
                            {"dangerous", true}, // 一律。annotations 不可信（规范原话）
                            {"server", c.name},
                            {"remote_name", remote}};
            lease.tools.push_back(std::move(one));
        }
    }
    return lease;
}

} // namespace realagent
