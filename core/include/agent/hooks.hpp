/*
 * hooks.hpp — 在 core 生命周期的几个点上跑外部命令（ADR-0024 §6）
 *
 * 装即授权：不经 permission、不走审批。只从 `hooks/hooks.json` 读，
 * 形状照抄 Claude Code：`{"hooks": {"<事件>": [{"matcher": "...", "hooks": [{"command": "..."}]}]}}`。
 * 没装 hook 时 run 立即返回空结果。
 */
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

namespace realagent {

enum class HookEvent {
    SessionStart,     // agent 创建 / /new / /resume 之后
    UserPromptSubmit, // 从收件箱取出一条消息时
    PreToolUse,       // 执行工具之前
    PostToolUse,      // 工具产出结果之后（被中断不算）
    Stop,             // 收工判定说到头了。只观察
};

struct HookOutcome {
    /* 只能收紧：hook 说 deny 就 deny，永远不能把 ask 变成 allow。
     * PostToolUse / Stop 上的 deny 拦不住任何东西，报一条后忽略。 */
    bool deny = false;
    std::string reason;
    std::string inject; // 要加进上下文的文字（SessionStart / UserPromptSubmit）
};

class Hooks {
  public:
    /* 走扫描链读 hooks.json。坏条目跳过，原因进 errors()。 */
    static Hooks scan(const std::string &workdir);

    bool empty() const { return by_event_.empty(); }
    const std::vector<std::string> &errors() const { return errors_; }

    /* 顺序跑这个事件上匹配的全部 hook，合并结果。
     * payload 里的 `matcher_key` 拿来跟 matcher（正则）比，不进 hook 的 stdin。
     * 超时 / 非零退出：报一条，继续。退出码 2 或 `{"decision":"block"}` 是 deny。
     * abort 变真时杀掉 hook 进程。 */
    HookOutcome run(HookEvent e, nlohmann::json payload,
                    const std::atomic<bool> *abort = nullptr) const;

  private:
    struct Entry {
        std::string matcher; // 空 = 全匹配
        std::string command;
        int timeout_ms = 10000;
        std::string from; // 哪个 plugin 带来的
    };
    std::map<HookEvent, std::vector<Entry>> by_event_;
    std::vector<std::string> errors_;
};

/* payload 超过这个大小就去掉大字段并标 truncated。 */
inline constexpr size_t kHookPayloadCap = 64 * 1024;

} // namespace realagent
