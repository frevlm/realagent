/*
 * plugin.hpp — plugin：一个安装单元，core 眼里唯一的容器（ADR-0024）
 *
 * `~/.realagent/` 与 `<workdir>/.realagent/` 本身也是 plugin（隐式的，没有前缀）。
 * 扫描器只面对一个有序的 plugin 列表；每一站的成员路径分别填好，
 * 装来的用 Claude Code 的文件名，隐式的用本项目的，下游不需要分辨。
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace realagent {

/* 唯一被展开的变量，名字照抄 Claude Code。 */
inline constexpr const char *kPluginRootVar = "${CLAUDE_PLUGIN_ROOT}";

/* 把 s 里全部 ${CLAUDE_PLUGIN_ROOT} 换成 root。root 空（隐式 plugin）而 s 里出现了它，
 * 返回 false。 */
bool expand_plugin_root(std::string &s, const std::filesystem::path &root);

/* 扫描链上的一站。root 空 = 隐式 plugin：没有前缀，也没有 plugin root 可展开。 */
struct PluginRoot {
    std::string name; // "user" / "project" / 目录名
    std::filesystem::path root;
    std::filesystem::path skills_dir;
    std::filesystem::path commands_dir;
    std::filesystem::path agents_dir;
    std::filesystem::path hooks_file;
    std::filesystem::path mcp_file;

    std::string prefix() const { return root.empty() ? std::string() : name; }

    /* 给模型看的名字：`<前缀>:<原名>`，隐式的就是原名。 */
    std::string qualify(const std::string &n) const
    {
        return root.empty() ? n : name + ":" + n;
    }
};

/* 远的在前、近的在后；调用方顺序遍历、后来者覆盖，就是「近的赢」。
 *
 *   ~/.realagent/plugins/<name>/          装来的，按目录名排序
 *   <workdir>/.realagent/plugins/<name>/  仓库带的
 *   ~/.realagent/            → "user"     隐式
 *   <workdir>/.realagent/    → "project"  隐式
 *
 * 前缀不同的两站撞不上名字，所以装来的不会盖掉自己写的。目录存不存在这里不判。 */
std::vector<PluginRoot> plugin_roots(const std::string &workdir);

/* `/plugins` 的一行。只读：卸载就是删目录。 */
struct PluginInfo {
    std::string name;
    std::string description; // .claude-plugin/plugin.json 里的
    std::string version;
    std::string root;
    bool implicit = false;
    int skills = 0;
    int commands = 0;
    int agent_defs = 0;
    int mcp_servers = 0;
    int hooks = 0;
};

/* 只数不解析：坏文件已由各自的扫描器在建 agent 时报过。 */
std::vector<PluginInfo> plugin_infos(const std::string &workdir);

} // namespace realagent
