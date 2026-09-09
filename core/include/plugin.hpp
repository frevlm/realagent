/*
 * plugin.hpp — plugin：一个安装单元，也是 core 眼里唯一的容器（ADR-0024）
 *
 * `~/.realagent/` 与 `<workdir>/.realagent/` 本身就是 plugin，只是没有名片的那两个
 * （**隐式 plugin**）。于是「两处来源」这个说法作废，代码里只剩一种东西：
 * 一个有序的 plugin 列表。
 *
 * 从前 skill 与 MCP 两个扫描器各自写死同样那两个路径，抄了两遍；合并规则也一样
 * （远的先扫，后来者赢）。现在两边遍历同一个列表，那条规则只写在一处。
 *
 * 每一站的成员路径**分别填**，不是「一个根 + 拼固定子路径」：装来的 plugin 用
 * Claude Code 的文件名（`skills/`、`.mcp.json`），隐式的用本项目现有的
 * （`.realagent/skills/`、`.realagent/mcp.json`）。这一步消掉了那个特殊情况——
 * 下游的扫描器一个 if 都没有。
 *
 * 扫盘在 agent 创建时发生一次，与 workdir 同期确定、同期不变（ADR-0022 §3）：
 * 不在 build_dialog 里扫，那会让一趟中间 system prompt 变形，prompt cache 当场碎掉。
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace realagent {

/* 唯一被展开的那个变量（ADR-0024 §4）。名字照抄 Claude Code——现成的 plugin 里
 * 写死的就是它，改一个字母就等于吃不下任何一个。 */
inline constexpr const char *kPluginRootVar = "${CLAUDE_PLUGIN_ROOT}";

/* 把 s 里**全部** `${CLAUDE_PLUGIN_ROOT}` 换成 root。
 *
 * root 空 = 隐式 plugin，那儿没有 plugin root：串里出现它就返回 false，
 * 调用方报一条人话。没出现就返回 true（什么都没做也是成功）。
 *
 * 全部替换，不是第一处——一条参数里写两次它是合法的，只换第一处会安静地留下
 * 一个字面量，而那正是这个函数存在的理由（ADR-0023 §2 骂过的静默丢弃）。 */
bool expand_plugin_root(std::string &s, const std::filesystem::path &root);

/* 扫描链上的一站。
 *
 * **`root` 空 = 隐式 plugin**，一个字段同时回答两件事：它有没有前缀，以及
 * `${CLAUDE_PLUGIN_ROOT}` 展开成什么。两个字段各存一份的话，它们会有不一致的那天。 */
struct PluginRoot {
    std::string name;                   // "user" / "project" / 目录名
    std::filesystem::path root;         // 装来的 plugin 的根目录；隐式的为空
    std::filesystem::path skills_dir;   // 这一站的 skill 目录
    std::filesystem::path commands_dir; // 这一站的 prompt 命令目录
    std::filesystem::path agents_dir;   // 这一站的 agent 定义目录
    std::filesystem::path hooks_file;   // 这一站的 hooks.json
    std::filesystem::path mcp_file;     // 这一站的 MCP 配置

    /* 装来的前缀是它的目录名，隐式的为空——于是既有的名字一个字符都不变，
     * 而第三方之间靠隔离消歧，不靠覆盖（ADR-0024 §3）。 */
    std::string prefix() const { return root.empty() ? std::string() : name; }

    /* 给模型看的名字：`<前缀>:<原名>`，隐式的就是原名本身。
     * skill 与 command 共用这一条——两处各拼一遍就会有拼得不一样的那天。 */
    std::string qualify(const std::string &n) const
    {
        return root.empty() ? n : name + ":" + n;
    }
};

/* 按 agent 的工作目录取，不按 core 的 cwd——core 是全机单实例，它的 cwd 与任何 agent 无关。
 *
 * **远的在前，近的在后**：调用方顺序遍历，后来者盖掉先到者，「同名近的覆盖远的」
 * 就是这个顺序的后果，不是另写的一条规则。
 *
 *   ~/.realagent/plugins/<name>/          装来的，按目录名排序
 *   <workdir>/.realagent/plugins/<name>/  仓库带的，可进版本库
 *   ~/.realagent/            → "user"     隐式，跟着人走
 *   <workdir>/.realagent/    → "project"  隐式，跟着仓库走
 *
 * 顺序只在**前缀相同的两站之间**起作用：`~/.realagent/plugins/foo` 与
 * `<workdir>/.realagent/plugins/foo`（同一个 plugin 的两份），以及隐式的 user 与 project。
 * **跨前缀撞不上**——那正是前缀存在的理由，所以「装来的会不会盖掉我自己写的」
 * 这个问题在这里不存在，不需要为它排序。
 *
 * 目录存不存在这里不判——判了也只是把同一件事在两处各做一遍，扫描器本来就要面对
 * 「这个目录不存在」（多数人一个 skill 都没有）。
 *
 * **`plugin.json` 不读**：它只是名片（ADR-0024 §2），今天没有任何消费者。
 * 名字取目录名，文件系统已经保证唯一。 */
std::vector<PluginRoot> plugin_roots(const std::string &workdir);

/* `/plugins` 那张表的一行：装了哪个、在哪儿、带了什么。
 *
 * **只读**。没有 enable / disable——那正是 [[ADR-0016]] 铲掉的 `plugins.disabled`；
 * 关掉一个 plugin 就是把目录删了。 */
struct PluginInfo {
    std::string name;
    std::string description; // .claude-plugin/plugin.json 里的；没有就是空串
    std::string version;     // 同上
    std::string root;        // 绝对路径；隐式 plugin 是它那个 .realagent 目录
    bool implicit = false;   // true = `user` / `project`，前缀为空
    int skills = 0;
    int commands = 0;
    int agent_defs = 0;
    int mcp_servers = 0;
    int hooks = 0;
};

/* 走一遍扫描链，数一数每一站带了什么。**只数不解析**：坏文件的原话由各自的扫描器
 * 在 agent 创建时报过了，这里再报一遍就是同一件事说两遍。
 *
 * `plugin.json` 在这里读——它是名片，而这是唯一一个看名片的地方（ADR-0024 §2）。 */
std::vector<PluginInfo> plugin_infos(const std::string &workdir);

} // namespace realagent
