#include "agent/command.hpp"

#include "agent/catalog.hpp"
#include "agent/session.hpp"
#include "llm/llm.hpp"
#include "plugin.hpp"

namespace realagent {

std::string command_error(const std::string &msg)
{
    return nlohmann::json{{"ok", false}, {"error", msg}}.dump();
}

/* /model 响应：模型数据表里的清单，current 标出配置里当前那档（ADR-0009）。 */
static nlohmann::json models_payload(const CoreContext &ctx)
{
    const std::string cur = ctx.config->model(ModelTier::Main);
    nlohmann::json arr = nlohmann::json::array();
    for (nlohmann::json m : ctx.pricing->models())
    {
        m["current"] = (m["name"] == cur);
        arr.push_back(std::move(m));
    }
    return arr;
}

nlohmann::json sessions_payload(const Agents &pool, const std::string &client,
                                const std::string &workdir)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const auto &s : Session::list(sessions_dir(workdir)))
    {
        nlohmann::json e = s;
        e["state"] = pool.state(client, s.id);
        arr.push_back(std::move(e));
    }
    return arr;
}

/* 查不到就只回名字——模型清单是参考资料，不是白名单（ADR-0009）。 */
nlohmann::json statusline_payload(const CoreContext &ctx)
{
    const std::string name = ctx.config->model(ModelTier::Main);
    nlohmann::json out;
    out["model"] = name;
    for (const nlohmann::json &m : ctx.pricing->models())
    {
        if (m["name"] != name) continue;
        out["owned_by"] = m["owned_by"];
        out["context"] = m["context"];
        break;
    }
    return out;
}

namespace {

/* 一条命令跑起来要够到的东西。agent 可能为空：这段对话还没开过。 */
struct Env {
    CoreContext &ctx;
    const std::string &workdir;
    const Agent *agent;
};

/* 成功载荷：{"ok":true,"command":name,"data":data} */
std::string ok_json(const char *name, nlohmann::json data)
{
    return nlohmann::json{{"ok", true}, {"command", name}, {"data", std::move(data)}}.dump();
}

std::string cmd_model(Env &e, const std::string &arg)
{
    // 无参 = 列清单；带名 = 切主模型并写回 settings.json。只认模型表里有的
    if (!arg.empty())
    {
        bool known = false;
        for (const nlohmann::json &m : models_payload(e.ctx))
            if (m["name"] == arg) known = true;
        if (!known) return command_error("unknown model: " + arg);
        // statusline 帧由事件循环发现变化后推
        if (!e.ctx.config->persist("model", nlohmann::json(arg)))
            return command_error("写入 settings.json 失败");
    }
    return ok_json("model", models_payload(e.ctx));
}

/* /plugins：装了哪些、各带了什么、建 agent 时出了什么问题。只读。 */
std::string cmd_plugins(Env &e, const std::string &)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const PluginInfo &p : plugin_infos(e.workdir))
    {
        // 什么都没带的隐式 plugin 不列
        if (p.implicit && !p.skills && !p.commands && !p.agent_defs && !p.mcp_servers && !p.hooks)
            continue;
        arr.push_back(nlohmann::json{{"name", p.name},
                                     {"description", p.description},
                                     {"version", p.version},
                                     {"root", p.root},
                                     {"implicit", p.implicit},
                                     {"skills", p.skills},
                                     {"commands", p.commands},
                                     {"agents", p.agent_defs},
                                     {"mcp_servers", p.mcp_servers},
                                     {"hooks", p.hooks}});
    }
    nlohmann::json errors = nlohmann::json::array();
    if (e.agent)
        for (const std::string &m : e.agent->plugin_errors()) errors.push_back(m);
    return ok_json("plugins", nlohmann::json{{"plugins", arr}, {"errors", errors}});
}

struct CommandDef {
    const char *name; // 不带前导 '/'
    const char *description;
    std::string (*run)(Env &, const std::string &arg);
};

/* 全部命令。清单与派发都读这张表，加一条命令就是加一行。 */
constexpr CommandDef kCommands[] = {
    {"model", "查看模型清单（/model <name> 切换主模型）", cmd_model},
    {"plugins", "查看装了哪些 plugin、各带了什么（只读；装 = git clone 进目录，卸 = 删掉）",
     cmd_plugins},
};

} // namespace

bool is_builtin_command(const std::string &name)
{
    if (name == "new" || name == "resume") return true; // 客户端就地处理，从不发到 core
    for (const CommandDef &c : kCommands)
        if (name == c.name) return true;
    return false;
}

nlohmann::json command_defs(const std::string &workdir)
{
    nlohmann::json arr = nlohmann::json::array();
    for (const CommandDef &c : kCommands)
        arr.push_back(
            nlohmann::json{{"name", c.name}, {"description", c.description}, {"kind", "builtin"}});
    for (const PromptCommand &c : scan_commands(workdir))
        arr.push_back(nlohmann::json{{"name", c.name},
                                     {"description", c.description},
                                     {"argument_hint", c.argument_hint},
                                     {"kind", "prompt"}});
    return arr;
}

CommandOutcome run_command(CoreContext &ctx, const std::string &workdir, const Agent *agent,
                           const std::string &input)
{
    // 首空白分词为命令名：/model[ <name>]
    const std::string cmd = input.substr(0, input.find(' '));
    // 命令参数：命令名之后去掉尾部空白的那一段（无参即空串）
    std::string arg = input.size() > cmd.size() ? input.substr(cmd.size() + 1) : std::string();
    while (!arg.empty() && arg.back() == ' ') arg.pop_back();
    const std::string name = cmd.substr(1); // 去掉前导 '/'

    for (const CommandDef &c : kCommands)
        if (name == c.name)
        {
            Env env{ctx, workdir, agent};
            return {.reply = c.run(env, arg)};
        }
    for (const PromptCommand &c : scan_commands(workdir))
        if (name == c.name) return {.post = expand_arguments(c.body, arg)};
    return {.reply = command_error("unknown command: " + cmd)};
}

} // namespace realagent
