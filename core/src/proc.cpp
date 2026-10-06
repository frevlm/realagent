#include "proc.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>

namespace realagent {

namespace {

using Clock = std::chrono::steady_clock;

void close_all(std::initializer_list<int> fds)
{
    for (int fd : fds)
        if (fd >= 0) close(fd);
}

/* 先 TERM 给它收尾的机会，1 秒还在就 KILL。返回 waitpid 的状态。 */
int kill_group(pid_t pid)
{
    kill(-pid, SIGTERM);
    int st = 0;
    for (int i = 0; i < 100; ++i)
    {
        if (waitpid(pid, &st, WNOHANG) > 0) return st;
        usleep(10000);
    }
    kill(-pid, SIGKILL);
    waitpid(pid, &st, 0);
    return st;
}

} // namespace

ProcResult run_proc(const ProcSpec &s)
{
    ProcResult r;
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1};
    if (pipe(in) || pipe(out) || pipe(err))
    {
        close_all({in[0], in[1], out[0], out[1], err[0], err[1]});
        r.fail = "pipe failed";
        return r;
    }

    const pid_t pid = fork();
    if (pid == 0)
    {
        // 子进程：多线程 fork 之后只能用 async-signal-safe 的调用
        setpgid(0, 0); // 自成进程组：杀的时候连子孙一起杀
        if (!s.cwd.empty() && chdir(s.cwd.c_str()) != 0) _exit(126);
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        dup2(s.merge_stderr ? out[1] : err[1], STDERR_FILENO);
        close_all({in[0], in[1], out[0], out[1], err[0], err[1]});
        execl("/bin/sh", "sh", "-c", s.command.c_str(), (char *)nullptr);
        _exit(127);
    }
    close_all({in[0], out[1], err[1]});
    if (pid < 0)
    {
        close_all({in[1], out[0], err[0]});
        r.fail = "fork failed";
        return r;
    }
    setpgid(pid, pid); // 父子各设一遍，谁先跑到都算数

    // 写不进去（命令压根不读 stdin）不是错
    signal(SIGPIPE, SIG_IGN);
    for (size_t at = 0; at < s.input.size();)
    {
        const ssize_t n = write(in[1], s.input.data() + at, s.input.size() - at);
        if (n <= 0) break;
        at += (size_t)n;
    }
    close(in[1]);

    const auto deadline = Clock::now() + std::chrono::milliseconds(s.timeout_ms);
    std::string line; // 还没凑齐一行的 stdout
    const auto take_out = [&](const char *p, size_t n) {
        if (r.out.size() + n > s.max_out)
        {
            r.truncated = true;
            return;
        }
        r.out.append(p, n);
        if (!s.on_line) return;
        line.append(p, n);
        for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1))
            s.on_line(std::string_view(line).substr(0, nl + 1));
    };

    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    while (fds[0].fd >= 0 || fds[1].fd >= 0)
    {
        if (s.abort && s.abort->load())
        {
            r.fail = "被中断";
            break;
        }
        if (s.timeout_ms > 0 && Clock::now() >= deadline)
        {
            r.fail = "超时（" + std::to_string(s.timeout_ms) + "ms）";
            break;
        }
        // 100ms 一轮：中断与超时要能被看见
        if (poll(fds, 2, 100) < 0) break;
        for (int i = 0; i < 2; ++i)
        {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP))) continue;
            char buf[4096];
            const ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n <= 0)
            {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
            else if (i == 0)
                take_out(buf, (size_t)n);
            else
                r.err.append(buf, (size_t)n);
        }
    }
    close_all({fds[0].fd, fds[1].fd});
    if (!line.empty() && s.on_line) s.on_line(line); // 末行没有换行符

    int st = 0;
    if (!r.fail.empty())
        st = kill_group(pid);
    else
        waitpid(pid, &st, 0);
    if (r.fail.empty()) r.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return r;
}

} // namespace realagent
