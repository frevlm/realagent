/*
 * hooks.cpp — 读 hooks.json，按事件跑匹配的命令，合并结果
 */
#include "agent/hooks.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>

#include "plugin.hpp"
#include "proc.hpp"

namespace realagent {

namespace fs = std::filesystem;

namespace {

/* 事件名照抄 Claude Code，现成的 hooks.json 才吃得下。 */
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

/* matcher 是正则。写坏了按不匹配处理。 */
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
    if (it == by_event_.end()) return out;

    const std::string key = payload.value("matcher_key", std::string());
    payload.erase("matcher_key"); // core 挑 hook 用的，不给 hook
    payload["hook_event_name"] = event_name(e);
    std::string text = payload.dump();
    if (text.size() > kHookPayloadCap)
    {
        payload["truncated"] = true;
        for (const char *k : {"tool_response", "tool_input", "prompt"}) payload.erase(k);
        text = payload.dump();
    }

    for (const Entry &h : it->second)
    {
        if (!matches(h.matcher, key)) continue;
        const ProcResult r = run_proc({.command = h.command, .input = text, .timeout_ms = h.timeout_ms, .abort = abort});
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

        // deny 一票定；拦不住东西的位置上说 deny，报一条
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
