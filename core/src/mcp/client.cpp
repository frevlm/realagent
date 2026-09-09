/*
 * client.cpp — 起进程、收发行、按 id 认领
 *
 * 一行一条 JSON-RPC 消息，这是 stdio 传输的全部帧格式（规范：消息以换行分隔，
 * 消息内不得含换行）。所以"框架"只有一个 split('\n')，剩下的是所有权和线程。
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
constexpr int kCallTimeoutMs = 300000;   // 一次 tools/call。拍的数，见 ADR-0023 §9
constexpr int kAbortPollMs = 100;        // 多久看一眼中止位

/* 管道四端一律 CLOEXEC。**不设这个，两个 server 就互相吊死**：起 B 的时候 B 继承了
 * A 的管道两端，于是 A 的 stdout 写端永远有人握着——A 退出了，我们的读线程也等不到
 * EOF，join() 不返回。子进程里 dup2 出来的 0/1 不带 CLOEXEC，正是我们要留给它的那两个。 */
void set_cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

/* 一串 string → execvp 要的 `char *const[]`（末尾 nullptr）。指向的仍是那串 string。 */
std::vector<char *> to_c_array(std::vector<std::string> &v)
{
    std::vector<char *> out;
    out.reserve(v.size() + 1);
    for (std::string &s : v) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

/* 每个请求都要带的那三样（ADR-0023 §5）。
 * protocolVersion 与 clientCapabilities 是必需的，clientInfo 是 SHOULD。
 * **clientCapabilities 是个空对象，而那就是全部的能力故事**——
 * 规范禁止 server 索取客户端没声明的东西。 */
nlohmann::json request_meta()
{
    return {{"io.modelcontextprotocol/protocolVersion", kMcpProtocolVersion},
            {"io.modelcontextprotocol/clientCapabilities", nlohmann::json::object()},
            {"io.modelcontextprotocol/clientInfo",
             {{"name", "realagent"}, {"version", "0.1"}}}};
}

/* JSON-RPC 错误对象 → 一句人话。带上 code：模型改不了它，但看得见"这不是我参数写错了"。 */
std::string rpc_error_text(const nlohmann::json &err)
{
    const std::string msg = err.value("message", std::string("unknown error"));
    const long long code = err.value("code", 0LL);
    std::string s = "MCP error " + std::to_string(code) + ": " + msg;
    // -32022 会在 data.supported 里列出它说得了哪些版本；那句话是给人看的，一并带上
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

    /* 子进程的环境：core 自己的，加上配置里的覆盖。**在父进程里拼好**——
     * fork 之后再 setenv 要 malloc，那不是 async-signal-safe 的。
     * 子进程里只做一次指针赋值（environ = ...），execvp 用的就是它，
     * 于是既有 PATH 查找又有自定义环境，两边平台都行。 */
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
    /* argv 直接指向 cfg 里的字符串——json 的字符串就是 std::string，取引用不拷贝，
     * 而 cfg 活得比这次调用久。execvp 的签名是历史遗留的 char* const[]，const_cast 到此为止。 */
    std::vector<char *> envp = to_c_array(envs);
    const std::string &command = cfg.at("command").get_ref<const std::string &>();
    std::vector<char *> argv{const_cast<char *>(command.c_str())};
    for (const auto &a : cfg.at("args"))
        argv.push_back(const_cast<char *>(a.get_ref<const std::string &>().c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid == 0)
    {
        /* 子进程：以下都是 async-signal-safe 的 */
        setpgid(0, 0); // 自成进程组：收尾时一枪打掉整棵子孙树
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        /* stderr **不接管**：规范说 server 可以往 stderr 写任何日志，
         * 且客户端"SHOULD NOT assume stderr output indicates error conditions"。
         * 让它直接流到 core 的 stderr——想看的人看得见，我们一个字都不解释。 */
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

    /* 没有握手。第一句话就是我们真正要的那一句。
     * 分页跟到底——不跟的话，工具多的 server 只看得见第一页。 */
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
            err = rerr; // 是哪个 server 由 hub 冠名——规格里已经没有名字了
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
    /* 规范给的收尾顺序：关掉它的 stdin，等它自己退，还赖着才动手。
     * "Servers SHOULD exit promptly when their standard input is closed"——
     * 这是唯一可移植的优雅退出信号。 */
    close(in_fd_);

    /* 等它死，最多 ms 毫秒。退出码没人要——这个进程的产出全在管道里，不在状态码里。 */
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
    /* 进程没了 → stdout 到 EOF → 读线程自己退。不需要 poll、不需要自管道。 */
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
            /* 一行不是 JSON、或者是 JSON 但不是对象（`null`、`3`、`"hi"` 都是合法 JSON），
             * 就不是 MCP 消息。**必须在这里挡住**：往下的 find/迭代器操作对非对象是要抛的，
             * 而这是一条线程，异常逃出去就是整个 core `terminate`——
             * 一个第三方进程吐错一行不该有这个能耐。 */
            if (msg.is_discarded() || !msg.is_object()) continue;
            /* **按 id 认领，不按到达顺序。** 通知（没有 id）和响应共用这一条流，
             * 谁先到不代表谁是谁的答案。我们没订阅任何通知（不发 subscriptions/listen），
             * 所以通知一律丢掉。 */
            const auto id = msg.find("id");
            if (id == msg.end() || !id->is_number_unsigned()) continue;
            /* 先把 id 取成一个值再动 msg。**不要合成一句** `done_[id->get<uint64_t>()]
             * = std::move(msg)`：右操作数先求值，msg 会在左边那个 id->get() 之前被移空，
             * 迭代器落在 null 上抛 invalid_iterator。 */
            const uint64_t rid = id->get<uint64_t>();
            std::lock_guard<std::mutex> lk(mtx_);
            done_[rid] = std::move(msg);
            cv_.notify_all();
        }
    }
    std::lock_guard<std::mutex> lk(mtx_);
    closed_ = true;
    cv_.notify_all(); // 还在等的人该醒了，等下去也等不到
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
            /* 分片等：中止位是别的线程置的，没有可等的条件变量。100ms 一看——
             * 这是人按 Esc 的尺度，不是热路径。 */
            cv_.wait_until(lk, std::min(deadline, now + std::chrono::milliseconds(kAbortPollMs)));
        }
        if (reply.is_null())
        {
            /* 放弃了。按规范办：发 notifications/cancelled，然后**无视迟到的响应**。
             * 不杀进程——连接是进程级共享的，杀了会把别的 agent 一起弄断。
             * 放弃的原因在上面就定下了，不留给调用方去重新猜一次：那要再读一遍
             * 同一个原子量的另一个时刻，超时之后按下的 Esc 会被报成「中止」。 */
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

    /* 到这儿为止，调用方拿到的一定是一个已经确认过的 complete result——
     * 「有 error 就转人话」「resultType 只认 complete」这两条判断只写这一处。 */
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
    /* resultType 只认 complete。input_required 与 task 出不来——前者要 client 声明能力，
     * 后者要声明 tasks 扩展，我们两个都没声明。其余一律非法（规范原话：
     * "A resultType of any value unrecognized by the client MUST be considered invalid"）。
     * 旧纪元的 server 没有这个字段，而旧纪元我们本来就不支持。 */
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
    /* 原样交出去。**不投影、不压平**——能不能带图片是端点协议的事，
     * 那个决定属于 llm/upstream/<协议>.cpp，不属于这里（ADR-0023 §3）。 */
    nlohmann::json out;
    out["content"] = res->value("content", nlohmann::json::array());
    out["isError"] = res->value("isError", false);
    return out;
}

} // namespace realagent
