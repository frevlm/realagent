/*
 * proc.hpp — 跑一条 /bin/sh -c 命令：喂 stdin、收输出、超时与中断时杀整个进程组
 *
 * bash 工具与 hook 共用。pid 只活在一次调用里，所以多个 agent 并发跑互不相干；
 * 中断是调用方传进来的那个 abort 标志，不是全局状态。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <string>
#include <string_view>

namespace realagent {

struct ProcSpec {
    std::string command;                                 // 交给 /bin/sh -c
    std::string cwd;                                     // 空 = 继承 core 的 cwd
    std::string input;                                   // 写进 stdin 后关闭（脚本常常读到 EOF 才动手）
    bool merge_stderr = false;                           // stderr 并进 stdout：交错顺序就是终端里看见的顺序
    int timeout_ms = 0;                                  // 0 = 不限
    size_t max_out = std::numeric_limits<size_t>::max(); // stdout 超过就只读不存
    const std::atomic<bool> *abort = nullptr;            // 变真即杀
    std::function<void(std::string_view)> on_line;       // stdout 每凑齐一行（含 \n）调一次
};

struct ProcResult {
    int status = -1; // 退出码；被信号杀为 128+信号；没跑成 / 超时 / 被中断为 -1
    std::string out;
    std::string err;
    bool truncated = false; // stdout 超过 max_out
    std::string fail;       // 非空 = 没跑成 / 超时 / 被中断
};

/* 阻塞到进程结束。中断或超时：先 SIGTERM 整组，1 秒不死再 SIGKILL。 */
ProcResult run_proc(const ProcSpec &spec);

} // namespace realagent
