/*
 * hooks.hpp — hook：在 core 生命周期的某个点上跑一个外部程序（ADR-0024 §6）
 *
 * **这是 plugin 里唯一一处 core 主动执行第三方代码。** 跟 MCP 分清楚：MCP 进入的是
 * 「工具」这个已经存在的洞——模型主动挑一个调；hook 是 core 在自己的生命周期点上自动跑。
 *
 * 但它**不是能力槽**（ADR-0011）：槽是「管线这一段由谁来干」，独占、必须有人填、
 * 填错管线就断；hook 是旁挂，可以零个，没有它管线照跑。
 *
 * **装即授权**：不经 permission、不走审批。用户把这个 plugin 放进目录，就是同意了
 * 它在这些点上跑——再问一遍是把一次明确的授权拆成每次都要重复的骚扰。
 *
 * 形状是**一个位置问题**，不是两个函数：
 *
 *     run(Pre*, payload)  →  原来那件事  →  run(Post*, payload)
 *
 * 一个 run，位置由事件名决定。写成 run_pre / run_post 两个函数的话，后者的返回值
 * 永远被忽略，那是个骗人的接口。**没装 hook 时 run 立即返回空 Outcome，零开销。**
 *
 * 只从 `hooks/hooks.json` 读，不内联 `plugin.json`——同一份数据两个落点，迟早只改一边。
 * 形状照抄 Claude Code：`{"hooks": {"<事件>": [{"matcher": "...", "hooks": [{"command": "..."}]}]}}`
 */
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "json.hpp"

namespace realagent {

/* **只在真有落点的地方成对。** 不为对称造名字——`SessionEnd` 在多 agent 里指哪件事
 * 答不上来，就别造。`PreCompact` 不适用：本项目没有压缩。 */
enum class HookEvent {
    SessionStart,     // agent 创建 / /new / /resume 之后
    UserPromptSubmit, // 从收件箱取出一条 user message 时
    PreToolUse,       // Executor::execute 之前
    PostToolUse,      // Executor::execute 之后（**只在真产出了结果时**）
    Stop,             // 收工判定说这趟到头了，agent_end 之前。只观察，不改控制流
};

struct HookOutcome {
    /* **只能收紧，不能放宽**：hook 说 deny 就 deny，说别的一律当没说过。
     * 于是没有优先级表，它退化成权限链上的一个「与」——一个 hook 永远不能把
     * ask 变成 allow。post 位置收到它要报一条，不静默丢。 */
    bool deny = false;
    std::string reason;
    std::string inject; // 要加进上下文的文字（SessionStart / UserPromptSubmit）
};

class Hooks {
  public:
    /* 走一遍扫描链读 hooks/hooks.json。坏条目跳过，原话进 errors()。 */
    static Hooks scan(const std::string &workdir);

    bool empty() const { return by_event_.empty(); }
    const std::vector<std::string> &errors() const { return errors_; }

    /* 跑这个事件上挂着的全部 hook，顺序执行，合并结果。
     *
     * payload 里的 `matcher_key`（工具名 / SessionStart 的来源）拿来跟 matcher 比，
     * matcher 为空就是全匹配。payload 会连同 `hook_event_name` 一起写进 hook 的 stdin。
     *
     * abort 变真时**直接杀**：hook 进程是这一个 agent 独占的，跟进程级共享的
     * MCP server 不是一回事，杀它不会弄断别人。
     *
     * 坏 hook 不许卡死 agent：超时 / 非零退出 → 报一条，继续跑。唯一的例外是它
     * 明确说了 deny（退出码 2，或 stdout 上一个 `{"decision":"block"}`）。 */
    HookOutcome run(HookEvent e, nlohmann::json payload,
                    const std::atomic<bool> *abort = nullptr) const;

  private:
    struct Entry {
        std::string matcher; // 空 = 全匹配
        std::string command; // 交给 /bin/sh -c
        int timeout_ms = 10000;
        std::string from; // 哪个 plugin 带来的，报错时冠名
    };
    std::map<HookEvent, std::vector<Entry>> by_event_;
    std::vector<std::string> errors_;
};

/* payload 里带工具结果的那些可能有几 MB。**截到定值并标 truncated，不静默截。** */
inline constexpr size_t kHookPayloadCap = 64 * 1024;

} // namespace realagent
