/*
 * client.cpp — 起进程、收发行、按 id 认领。stdio 传输一行一条 JSON-RPC 消息。
 */
#include "mcp/mcp.hpp"

#include "tools/tools.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

extern char **environ;

namespace realagent {

namespace {

constexpr int kStartupTimeoutMs = 30000; // 起进程 + 第一次 tools/list
constexpr int kCallTimeoutMs = 300000;   // 一次 tools/call（ADR-0023 §9）
constexpr int kAbortPollMs = 100;        // 多久看一眼中止位

/* 管道一律 CLOEXEC：否则后起的 server 继承前一个的管道，前一个退出时读线程等不到 EOF。 */
void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

/* string 数组 → execvp 要的 `char *const[]`（末尾 nullptr） */
std::vector<char *> to_c_array(std::vector<std::string> &v)
{
    std::vector<char *> out;
    out.reserve(v.size() + 1);
    for (std::string &s : v) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

/* 每个请求都带的 _meta（ADR-0023 §5）。能力为空：server 不会反过来索取任何东西。 */
nlohmann::json request_meta()
{
    return {{"io.modelcontextprotocol/protocolVersion", kMcpProtocolVersion},
            {"io.modelcontextprotocol/clientCapabilities", nlohmann::json::object()},
            {"io.modelcontextprotocol/clientInfo",
             {{"name", "realagent"}, {"version", "0.1"}}}};
}

/* JSON-RPC 错误 → 一句带 code 的人话 */
std::string rpc_error_text(const nlohmann::json &err)
{
    const std::string msg = err.value("message", std::string("unknown error"));
    const long long code = err.value("code", 0LL);
    std::string s = "MCP error " + std::to_string(code) + ": " + msg;
    // -32022（版本不符）在 data.supported 里列出 server 支持的版本
    if (const auto d = err.find("data"); d != err.end() && d->contains("supported"))
        s += " (server speaks: " + d->at("supported").dump() + ")";
    return s;
}

} // namespace

std::unique_ptr<McpClient> McpClient::start(const nlohmann::json &cfg, std::string &err)
{
    err.clear();
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0)
    {
        err = "pipe failed";
        return nullptr;
    }
    if (pipe(from_child) != 0)
    {
        close(to_child[0]);
        close(to_child[1]);
        err = "pipe failed";
        return nullptr;
    }
    for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1]}) set_cloexec(fd);

    /* 环境 = core 自己的 + 配置覆盖。在父进程里拼好：fork 之后 setenv 不是
     * async-signal-safe 的；子进程只赋一次 environ，execvp 照常做 PATH 查找。 */
    const nlohmann::json &env = cfg.at("env");
    std::vector<std::string> envs;
    for (char **e = environ; *e; ++e)
    {
        const std::string s(*e);
        const size_t eq = s.find('=');
        if (eq != std::string::npos && env.contains(s.substr(0, eq))) continue;
        envs.push_back(s);
    }
    for (const auto &[k, v] : env.items()) envs.push_back(k + "=" + v.get<std::string>());
    // argv 直接指向 cfg 里的字符串（cfg 活得比这次调用久）
    std::vector<char *> envp = to_c_array(envs);
    const std::string &command = cfg.at("command").get_ref<const std::string &>();
    std::vector<char *> argv{const_cast<char *>(command.c_str())};
    for (const auto &a : cfg.at("args"))
        argv.push_back(const_cast<char *>(a.get_ref<const std::string &>().c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid == 0)
    {
        setpgid(0, 0); // 自成进程组：收尾时连子孙一起收
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        // stderr 不接管：server 的日志直接流到 core 的 stderr
        close(to_child[0]);
        close(to_child[1]);
        close(from_child[0]);
        close(from_child[1]);
        environ = envp.data();
        execvp(command.c_str(), argv.data());
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    if (pid < 0)
    {
        close(to_child[1]);
        close(from_child[0]);
        err = "fork failed";
        return nullptr;
    }
    setpgid(pid, pid); // 父子各设一遍，谁先跑到都算数

    std::unique_ptr<McpClient> c(new McpClient());
    c->cfg_ = cfg;
    c->pid_ = pid;
    c->in_fd_ = to_child[1];
    c->out_fd_ = from_child[0];
    c->reader_ = std::thread([p = c.get()] { p->reader_loop(); });

    // 没有握手，直接 tools/list，分页跟到底
    nlohmann::json cursor;
    for (;;)
    {
        nlohmann::json params = nlohmann::json::object();
        if (!cursor.is_null()) params["cursor"] = cursor;
        std::string rerr;
        const auto res =
            c->request("tools/list", std::move(params), nullptr, kStartupTimeoutMs, rerr);
        if (!res)
        {
            err = rerr; // 由 hub 冠上名字
            return nullptr;
        }
        if (const auto tools = res->find("tools"); tools != res->end() && tools->is_array())
            for (const auto &one : *tools) c->tools_.push_back(one);
        const auto n = res->find("nextCursor");
        if (n == res->end() || n->is_null()) break;
        cursor = *n;
    }
    return c;
}

McpClient::~McpClient() { shutdown(); }

void McpClient::shutdown()
{
    // 规范的收尾顺序：关 stdin 等它自己退，2 秒后 TERM，再 1 秒 KILL
    close(in_fd_);

    const auto reap = [this](int ms) {
        for (int i = 0; i < ms / 10; ++i)
        {
            if (waitpid(pid_, nullptr, WNOHANG) > 0) return true;
            usleep(10000);
        }
        return false;
    };
    if (!reap(2000))
    {
        kill(-pid_, SIGTERM);
        if (!reap(1000))
        {
            kill(-pid_, SIGKILL);
            waitpid(pid_, nullptr, 0);
        }
    }
    // 进程没了 → stdout 到 EOF → 读线程自己退
    reader_.join();
    close(out_fd_);
}

void McpClient::reader_loop()
{
    std::string buf;
    char chunk[8192];
    for (;;)
    {
        const ssize_t n = read(out_fd_, chunk, sizeof chunk);
        if (n <= 0) break; // EOF 或出错：进程没了
        buf.append(chunk, (size_t)n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos)
        {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) continue;
            nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
            // 不是 JSON 对象就不是 MCP 消息。必须挡住：下面对非对象会抛，线程里抛出去就是 terminate
            if (msg.is_discarded() || !msg.is_object()) continue;
            // 按 id 认领；没有 id 的是通知，我们没订阅任何通知，丢掉
            const auto id = msg.find("id");
            if (id == msg.end() || !id->is_number_unsigned()) continue;
            // 先取出 id 再 move msg：合成一句的话右边先求值，迭代器会悬空
            const uint64_t rid = id->get<uint64_t>();
            std::lock_guard<std::mutex> lk(mtx_);
            done_[rid] = std::move(msg);
            cv_.notify_all();
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    closed_ = true;
    cv_.notify_all();
}

void McpClient::send_line(const nlohmann::json &msg)
{
    const std::string line = msg.dump() + "\n";
    std::lock_guard<std::mutex> lk(write_mtx_); // 一行不许被另一行插进去
    size_t off = 0;
    while (off < line.size())
    {
        const ssize_t w = write(in_fd_, line.data() + off, line.size() - off);
        if (w <= 0) return; // 写不进去 = 对面没了，等的那一方会撞上 closed_
        off += (size_t)w;
    }
}

std::optional<nlohmann::json> McpClient::request(const std::string &method, nlohmann::json params,
                                                 const std::atomic<bool> *abort, int timeout_ms,
                                                 std::string &err)
{
    const uint64_t id = next_id_.fetch_add(1);
    params["_meta"] = request_meta();
    send_line({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    nlohmann::json reply;
    {
        std::unique_lock<std::mutex> lk(mtx_);
        for (;;)
        {
            if (const auto it = done_.find(id); it != done_.end())
            {
                reply = std::move(it->second);
                done_.erase(it);
                break;
            }
            if (closed_)
            {
                err = "MCP server 没了（stdout 到了 EOF）";
                return std::nullopt;
            }
            if (abort && abort->load())
            {
                err = "interrupted by user";
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                err = "MCP server 超时未回应";
                break;
            }
            // 分片等：中止位没有可等的条件变量
            cv_.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(kAbortPollMs)));
        }
        if (reply.is_null())
        {
            // 放弃：发 notifications/cancelled，无视迟到的响应，不杀共享的进程
            done_.erase(id);
        }
    }
    if (reply.is_null())
    {
        send_line({{"jsonrpc", "2.0"},
                   {"method", "notifications/cancelled"},
                   {"params", {{"requestId", id}, {"reason", err}}}});
        return std::nullopt;
    }

    if (const auto e = reply.find("error"); e != reply.end())
    {
        err = rpc_error_text(*e);
        return std::nullopt;
    }
    const auto res = reply.find("result");
    if (res == reply.end() || !res->is_object())
    {
        err = "响应里既没有 result 也没有 error";
        return std::nullopt;
    }
    // 只认 complete：我们没声明能力与 tasks 扩展，别的值按规范一律非法
    if (res->value("resultType", std::string()) != "complete")
    {
        err = "resultType=" + res->value("resultType", std::string("(缺失，多半是旧纪元的 server)"));
        return std::nullopt;
    }
    return *res;
}

nlohmann::json McpClient::call(const std::string &name, const nlohmann::json &arguments,
                               const std::atomic<bool> *abort)
{
    nlohmann::json params;
    params["name"] = name;
    params["arguments"] = arguments.is_object() ? arguments : nlohmann::json::object();
    std::string err;
    const auto res = request("tools/call", std::move(params), abort, kCallTimeoutMs, err);
    if (!res) return tool_fail(err);
    // 原样交出，不压平：能不能带图片由 llm/upstream 决定
    nlohmann::json out;
    out["content"] = res->value("content", nlohmann::json::array());
    out["isError"] = res->value("isError", false);
    return out;
}

} // namespace realagent
