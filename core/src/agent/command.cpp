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

/* /model 清单：端点报的模型，表里认得的补上 owned_by / context（ADR-0009），current 标出主模型 */
static std::expected<nlohmann::json, std::string> models_payload(const CoreContext &ctx)
{
    const Config &cfg = *ctx.config;
    const auto ids = endpoint_models(
        {{"protocol", cfg.get("protocol")}, {"base_url", cfg.get("base_url")}, {"api_key", cfg.get("api_key")}});
    if (!ids) return std::unexpected("拉不到模型清单（" + ids.error() + "）。手动指定：/model <模型名>");
    const std::string cur = cfg.model(ModelTier::Main);
    nlohmann::json arr = nlohmann::json::array();
    for (const std::string &id : *ids)
    {
        nlohmann::json m{{"name", id}};
        for (const nlohmann::json &known : ctx.pricing->models())
            if (known["name"] == id)
            {
                m = known;
                break;
            }
        m["current"] = (id == cur);
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
    out["effort"] = ctx.config->get("effort");
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

CommandOutcome cmd_model(Env &e, const std::string &arg)
{
    if (arg.empty())
    {
        auto list = models_payload(e.ctx);
        return {.reply = list ? ok_json("model", std::move(*list)) : command_error(list.error())};
    }
    // 带名 = 切主模型，不查清单（手填）。statusline 帧由事件循环发现变化后推
    if (!e.ctx.config->persist("model", nlohmann::json(arg))) return {.reply = command_error("写入 settings.json 失败")};
    return {.reply = ok_json("model", nlohmann::json::array({{{"name", arg}, {"current", true}}}))};
}

/* /effort [level]：看或改主模型的思考量级。取值不查表，原样发给端点（ADR-0035）。
 * default = 写成空串：请求里不带这个字段，由端点用模型自己的默认档 */
CommandOutcome cmd_effort(Env &e, const std::string &arg)
{
    const std::string v = arg == "default" ? "" : arg;
    if (!arg.empty() && !e.ctx.config->persist("effort", nlohmann::json(v)))
        return {.reply = command_error("写入 settings.json 失败")};
    return {.reply = ok_json("effort", nlohmann::json{{"effort", e.ctx.config->get("effort")}})};
}

/* 一个可调键的当前状态：{key, description, choices, value} */
nlohmann::json tunable_state(const Config &cfg, nlohmann::json t)
{
    t["value"] = cfg.get_json(t["key"].get<std::string>());
    return t;
}

/* /config [key [value]]：看全部可调键 / 看一个键 / 改一个键。值按 choices 的写法认：
 * 字符串比原文，数字比 dump 出来的样子，所以 "80" 认作 80。只写 settings.json 里那一个键，
 * 内存同步改，下一次读就生效（ADR-0010 的点对点 persist）。 */
CommandOutcome cmd_config(Env &e, const std::string &arg)
{
    const Config &cfg = *e.ctx.config;
    const std::string key = arg.substr(0, arg.find(' '));
    const std::string text = key.size() < arg.size() ? arg.substr(key.size() + 1) : "";

    nlohmann::json items = nlohmann::json::array();
    for (const nlohmann::json &t : tunables())
        if (key.empty() || t["key"] == key) items.push_back(tunable_state(cfg, t));
    if (items.empty()) return {.reply = command_error("没有这个配置项：" + key)};
    if (text.empty()) return {.reply = ok_json("config", nlohmann::json{{"items", items}})};

    for (const nlohmann::json &c : items[0]["choices"])
    {
        if ((c.is_string() ? c.get<std::string>() : c.dump()) != text) continue;
        if (!e.ctx.config->persist(key, c)) return {.reply = command_error("写入 settings.json 失败")};
        items[0]["value"] = c;
        return {.reply = ok_json("config", nlohmann::json{{"items", items}})};
    }
    return {.reply = command_error(key + " 不能取 " + text + "，可取：" + items[0]["choices"].dump())};
}

/* /plugins：装了哪些、各带了什么、建 agent 时出了什么问题。只读。 */
CommandOutcome cmd_plugins(Env &e, const std::string &)
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
    return {.reply = ok_json("plugins", nlohmann::json{{"plugins", arr}, {"errors", errors}})};
}

/* /compact：压缩要动历史，只能在 agent 自己的线程上做，这里只把请求投过去 */
CommandOutcome cmd_compact(Env &, const std::string &arg) { return {.post = arg, .compact = true}; }

struct CommandDef {
    const char *name; // 不带前导 '/'
    const char *description;
    CommandOutcome (*run)(Env &, const std::string &arg);
};

/* 全部命令。清单与派发都读这张表，加一条命令就是加一行。 */
constexpr CommandDef kCommands[] = {
    {"model", "从端点拉模型清单（/model <name> 直接切换主模型）", cmd_model},
    {"effort", "主模型的思考量级（/effort <none|minimal|low|medium|high|xhigh|max|default>，端点认哪些看端点）", cmd_effort},
    {"plugins", "查看装了哪些 plugin、各带了什么（只读；装 = git clone 进目录，卸 = 删掉）",
     cmd_plugins},
    {"config", "查看与修改设置（/config <键> <值> 直接改，写进 settings.json 即刻生效）", cmd_config},
    {"compact", "把这段对话的历史压成一条摘要（/compact <指令> 告诉它重点留什么）", cmd_compact},
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
            return c.run(env, arg);
        }
    for (const PromptCommand &c : scan_commands(workdir))
        if (name == c.name) return {.post = expand_arguments(c.body, arg)};
    return {.reply = command_error("unknown command: " + cmd)};
}

} // namespace realagent
