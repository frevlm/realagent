/*
 * hooks.cpp — 读 hooks.json、起进程、收结果
 *
 * 进程这一套不复用 bash.cpp：那里为「同一时刻至多一个子进程」优化过，用的是一个全局 pid；
 * hook 会被多个 agent 同时触发，pid 必须是局部的。
 */
#include "agent/hooks.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>

#include "plugin.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 事件名照抄 Claude Code，一个字母都不改——改了就等于吃不下任何现成的 hooks.json。 */
const std::map<std::string, HookEvent> kEvents = {
    {"SessionStart", HookEvent::SessionStart},
    {"UserPromptSubmit", HookEvent::UserPromptSubmit},
    {"PreToolUse", HookEvent::PreToolUse},
    {"PostToolUse", HookEvent::PostToolUse},
    {"Stop", HookEvent::Stop},
};

const char *event_name(HookEvent e)
{
    for (const auto &[k, v] : kEvents)
        if (v == e) return k.c_str();
    return "";
}

/* matcher 是正则（Claude Code 就是这么用的）。写坏了不让整个 core 遭殃——
 * 编译失败就退化成「不匹配」并报一条，因为一个匹配不了的 matcher 的本意
 * 显然不是「匹配一切」。 */
bool matches(const std::string &matcher, const std::string &key)
{
    if (matcher.empty()) return true; // 没写 = 全匹配
    try
    {
        return std::regex_search(key, std::regex(matcher));
    } catch (const std::exception &)
    {
        return false;
    }
}

/* 一次执行的产出。 */
struct Run {
    int status = -1; // 退出码；-1 = 没跑成 / 超时 / 被杀
    std::string out;
    std::string err;
    std::string fail; // 非空 = 人话失败原因（超时、fork 失败……）
};

/* 起一个 /bin/sh -c，把 payload 写进它的 stdin，收 stdout / stderr。
 *
 * **stdin 必须关**：现成的 hook 脚本读 stdin 到 EOF 才动手（`readFileSync(0)`），
 * 不关它就永远等在那儿。这也正是「一次事件一次进程」的由来——想复用进程就得
 * 让 stdin 一直开着，那些脚本一个都跑不了。 */
Run exec_hook(const std::string &cmd, const std::string &stdin_text, int timeout_ms,
              const std::atomic<bool> *abort)
{
    Run r;
    int in_fd[2], out_fd[2], err_fd[2];
    if (pipe(in_fd) || pipe(out_fd) || pipe(err_fd))
    {
        r.fail = "pipe failed";
        return r;
    }
    const pid_t pid = fork();
    if (pid < 0)
    {
        for (int fd : {in_fd[0], in_fd[1], out_fd[0], out_fd[1], err_fd[0], err_fd[1]}) close(fd);
        r.fail = "fork failed";
        return r;
    }
    if (pid == 0)
    {
        // 子进程：以下都是 async-signal-safe 的，多线程 fork 之后只能用这些
        setpgid(0, 0); // 自成进程组：杀的时候连它派生的一起杀
        dup2(in_fd[0], STDIN_FILENO);
        dup2(out_fd[1], STDOUT_FILENO);
        dup2(err_fd[1], STDERR_FILENO);
        for (int fd : {in_fd[0], in_fd[1], out_fd[0], out_fd[1], err_fd[0], err_fd[1]}) close(fd);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)nullptr);
        _exit(127);
    }
    close(in_fd[0]);
    close(out_fd[1]);
    close(err_fd[1]);

    // 写 payload 再关：脚本读到 EOF 才会动手。写不进去（脚本压根没读）不是错
    signal(SIGPIPE, SIG_IGN);
    for (size_t at = 0; at < stdin_text.size();)
    {
        const ssize_t n = write(in_fd[1], stdin_text.data() + at, stdin_text.size() - at);
        if (n <= 0) break;
        at += (size_t)n;
    }
    close(in_fd[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool killed = false;
    pollfd fds[2] = {{out_fd[0], POLLIN, 0}, {err_fd[0], POLLIN, 0}};
    while (fds[0].fd >= 0 || fds[1].fd >= 0)
    {
        if (abort && abort->load())
        {
            r.fail = "被中断";
            killed = true;
            break;
        }
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  std::chrono::steady_clock::now())
                .count();
        if (left <= 0)
        {
            r.fail = "超时（" + std::to_string(timeout_ms) + "ms）";
            killed = true;
            break;
        }
        // 100ms 一轮：中断要能被看见，而 hook 本来就是短命的东西
        const int rc = poll(fds, 2, (int)std::min<long long>(left, 100));
        if (rc < 0) break;
        for (int i = 0; i < 2; ++i)
        {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP))) continue;
            char buf[4096];
            const ssize_t n = read(fds[i].fd, buf, sizeof buf);
            if (n > 0)
                (i == 0 ? r.out : r.err).append(buf, (size_t)n);
            else
            {
                close(fds[i].fd);
                fds[i].fd = -1;
            }
        }
    }
    for (pollfd &f : fds)
        if (f.fd >= 0) close(f.fd);

    if (killed) kill(-pid, SIGKILL); // 连它派生出来的一起杀
    int st = 0;
    waitpid(pid, &st, 0);
    if (!killed) r.status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    return r;
}

} // namespace

Hooks Hooks::scan(const std::string &workdir)
{
    Hooks h;
    for (const PluginRoot &r : plugin_roots(workdir))
    {
        const fs::path &path = r.hooks_file;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec)) continue;
        nlohmann::json doc;
        try
        {
            std::ifstream f(path);
            doc = nlohmann::json::parse(f);
        } catch (const nlohmann::json::exception &ex)
        {
            h.errors_.push_back(path.string() + ": " + ex.what()); // 库的原话带行号列号
            continue;
        }
        const nlohmann::json &groups = doc.contains("hooks") ? doc["hooks"] : doc;
        if (!groups.is_object())
        {
            h.errors_.push_back(path.string() + ": hooks 不是一个对象");
            continue;
        }
        for (const auto &[ev, arr] : groups.items())
        {
            const auto known = kEvents.find(ev);
            if (known == kEvents.end())
            {
                // 认不出的事件点名说，不静默丢：PreCompact 一类本项目就是没有
                h.errors_.push_back(path.string() + ": 事件 `" + ev + "` 本项目没有这个落点");
                continue;
            }
            if (!arr.is_array()) continue;
            for (const auto &g : arr)
            {
                const std::string matcher = g.value("matcher", std::string());
                for (const auto &one : g.value("hooks", nlohmann::json::array()))
                {
                    try
                    {
                        if (one.value("type", std::string("command")) != "command")
                            throw std::runtime_error("只认 type=\"command\"");
                        std::string cmd = one.at("command").get<std::string>();
                        if (!expand_plugin_root(cmd, r.root))
                            throw std::runtime_error(std::string(kPluginRootVar) +
                                                     " 只在 plugin 里有意义");
                        Entry e;
                        e.matcher = matcher;
                        e.command = std::move(cmd);
                        e.from = r.name;
                        if (one.contains("timeout"))
                            e.timeout_ms = (int)(one["timeout"].get<double>() * 1000);
                        h.by_event_[known->second].push_back(std::move(e));
                    } catch (const std::exception &ex)
                    {
                        h.errors_.push_back(path.string() + ": " + ex.what());
                    }
                }
            }
        }
    }
    return h;
}

HookOutcome Hooks::run(HookEvent e, nlohmann::json payload, const std::atomic<bool> *abort) const
{
    HookOutcome out;
    const auto it = by_event_.find(e);
    if (it == by_event_.end()) return out; // 没装：零开销，与加这个功能之前一个字不差

    const std::string key = payload.value("matcher_key", std::string());
    payload.erase("matcher_key"); // 那是 core 用来挑 hook 的，不该出现在给 hook 的 payload 里
    payload["hook_event_name"] = event_name(e);
    std::string text = payload.dump();
    if (text.size() > kHookPayloadCap)
    {
        // 截了就说，不静默——ADR-0023 §2 骂过的那件事
        payload["truncated"] = true;
        for (const char *k : {"tool_response", "tool_input", "prompt"}) payload.erase(k);
        text = payload.dump();
    }

    for (const Entry &h : it->second)
    {
        if (!matches(h.matcher, key)) continue;
        const Run r = exec_hook(h.command, text, h.timeout_ms, abort);
        if (!r.fail.empty())
        {
            // 坏 hook 不许卡死 agent：报一条，继续跑
            fprintf(stderr, "[hook] %s %s: %s\n", h.from.c_str(), event_name(e), r.fail.c_str());
            continue;
        }
        /* stdout 上一个 JSON 对象：认 decision / reason / additionalContext
         * （Claude Code 的字段名，照抄）。不是 JSON 就当成一段要注入的文字。 */
        const nlohmann::json j = nlohmann::json::parse(r.out, nullptr, false);
        std::string inject;
        bool deny = r.status == 2; // 退出码 2 = 拦下，这是 Claude Code 的约定
        std::string reason = deny ? r.err : std::string();
        if (j.is_object())
        {
            if (j.value("decision", std::string()) == "block")
            {
                deny = true;
                reason = j.value("reason", std::string());
            }
            inject = j.value("additionalContext", std::string());
            if (const auto hs = j.find("hookSpecificOutput"); hs != j.end() && hs->is_object())
                inject = hs->value("additionalContext", inject);
        }
        else if (r.status == 0)
        {
            inject = r.out;
        }
        if (r.status != 0 && r.status != 2 && !deny)
            fprintf(stderr, "[hook] %s %s: 退出码 %d\n", h.from.c_str(), event_name(e), r.status);

        /* **只能收紧，不能放宽**：deny 一票定，说别的一律当没说过。
         * post 位置收到 deny 也要报一条——那儿没人看它，静默丢是最坏的处理。 */
        if (deny)
        {
            if (e == HookEvent::PostToolUse || e == HookEvent::Stop)
                fprintf(stderr, "[hook] %s %s: 说了 deny，但这个位置拦不住任何东西，忽略\n",
                        h.from.c_str(), event_name(e));
            else
            {
                out.deny = true;
                if (!reason.empty()) out.reason = reason;
            }
        }
        if (!inject.empty())
        {
            if (!out.inject.empty()) out.inject += "\n";
            out.inject += inject;
        }
    }
    return out;
}

} // namespace realagent
