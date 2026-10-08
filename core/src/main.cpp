/*
 * main.cpp — realagent core 入口：读配置、建 agent 池、挂路由、监听
 */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "agent/agents.hpp"
#include "agent/command.hpp"
#include "agent/session.hpp"
#include "config.hpp"
#include "mcp/mcp.hpp"
#include "server/server.hpp"

using namespace realagent;
using nlohmann::json;

namespace {

/* 请求体解析成对象；不是 JSON 对象就当空对象。 */
json parse_body(const std::string &body)
{
    json j = json::parse(body, nullptr, false);
    return j.is_object() ? j : json::object();
}

/* 自己所在目录排进 PATH 最前：随 core 发的 realontext 在那里，search 与 bash 都找得到它（ADR-0027）。 */
void put_own_dir_on_path()
{
#if defined(__APPLE__)
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) != 0) return;
    const std::filesystem::path self(buf);
#else
    const std::filesystem::path self("/proc/self/exe");
#endif
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::canonical(self, ec).parent_path();
    if (ec) return;
    const std::string path = getenv_or("PATH", "");
    setenv("PATH", (dir.string() + (path.empty() ? "" : ":" + path)).c_str(), 1);
}

} // namespace

int main()
{
    put_own_dir_on_path(); // 早于任何线程：setenv 不是线程安全的

    auto loaded = Config::load();
    if (!loaded)
    {
        fprintf(stderr, "[config] %s\n", loaded.error().c_str());
        return 1;
    }
    Config cfg = std::move(*loaded);

    // 读不动模型表只是不计价，照常启动
    std::string price_err;
    const Pricing pricing = Pricing::load(cfg, &price_err);
    if (!price_err.empty()) fprintf(stderr, "[llm] %s（本次运行不计价）\n", price_err.c_str());

    McpHub mcp_hub;

    Server server;
    CoreContext ctx{.config = &cfg, .pricing = &pricing, .mcp = &mcp_hub};
    ctx.emit_fn = [&server](const std::string &type, const std::string &payload) {
        server.push_event(type, payload);
    };

    // 端点没配齐也照常起：这段话会原样回给每一条 POST /message，出现在用户眼前
    if (const std::string e = endpoint_config_error(cfg); !e.empty()) fprintf(stderr, "[config] %s\n", e.c_str());

    ApprovalCoordinator approval;
    approval.set_emit(ctx.emit_fn);
    Agents pool(ctx, approval);

    approval.set_online([&server] { return server.has_client(); });

    /* 按 agent_id 找到 agent 再交给 f；找不到回错误。 */
    const auto with_agent = [&pool](auto f) -> Handler {
        return [&pool, f](const std::string &body) {
            const json j = parse_body(body);
            const int id = j.value("agent_id", 0);
            Agent *a = pool.find(id);
            if (!a) return command_error("无此 agent: " + std::to_string(id));
            return f(*a, j);
        };
    };
    /* 请求那一队里不等锁：agent 正在跑就当场回 AGENT_BUSY。 */
    const auto when_idle = [&with_agent](auto f) -> Handler {
        return with_agent([f](Agent &a, const json &j) {
            auto lk = a.try_lock();
            return lk.owns_lock() ? f(a, j) : command_error(AGENT_BUSY);
        });
    };

    server.route("POST", "/agent", [&pool](const std::string &body) {
        std::string err;
        const int id = pool.create(parse_body(body).value("workdir", ""), 0, {}, {}, err);
        if (id <= 0) return command_error(err);
        return json{{"ok", true}, {"agent_id", id}}.dump();
    });
    server.route("GET", "/agents", [&pool](const std::string &) { return pool.list().dump(); });

    server.route("GET", "/commands", [&pool](const std::string &body) {
        // agent_id 可选：不带就只有内置命令
        return command_defs(pool.find(parse_body(body).value("agent_id", 0))).dump();
    });

    const Handler session_get = with_agent([](Agent &a, const json &) {
        return Session::read_frames(a.session_dir(), a.session_id()).dump();
    });
    server.route("GET", "/session", session_get);
    server.route("GET", "/history", session_get);

    server.route("POST", "/message", with_agent([&](Agent &a, const json &j) {
                     const std::string msg = j.value("message", "");
                     if (msg.empty()) return command_error("empty message");
                     if (msg[0] == '/') return handle_command(ctx, pool, a, msg);
                     // 每条现算：POST /setup 会在运行中把端点配齐
                     if (std::string e = endpoint_config_error(cfg); !e.empty()) return command_error(e);
                     a.post(msg);
                     return std::string(R"({"status":"processing"})");
                 }));
    server.route("POST", "/command", with_agent([&](Agent &a, const json &j) {
                     std::string cmd = j.value("command", "");
                     if (cmd.empty()) return command_error("empty command");
                     if (cmd[0] != '/') cmd.insert(cmd.begin(), '/');
                     return handle_command(ctx, pool, a, cmd);
                 }));

    server.route("GET", "/sessions", when_idle([&pool](Agent &a, const json &) {
                     return sessions_payload(pool, a).dump();
                 }));
    server.route("POST", "/session", when_idle([&pool](Agent &a, const json &j) {
                     const std::string sid = j.value("id", "");
                     if (sid.empty())
                         a.reset();
                     else if (!a.resume(sid))
                         return command_error("unknown session: " + sid);
                     return json{{"ok", true}, {"data", sessions_payload(pool, a)}}.dump();
                 }));

    server.route("POST", "/interrupt", [&](const std::string &body) {
        if (Agent *a = pool.find(parse_body(body).value("agent_id", 0)))
        {
            a->interrupt();
            approval.cancel(a->id());
        }
        return std::string(R"({"status":"ok"})");
    });
    server.route("POST", "/approval-response", [&approval](const std::string &body) {
        const json j = parse_body(body);
        approval.respond(j.value("id", ""), j.value("allow", false));
        return std::string(R"({"status":"ok"})");
    });
    // 客户端退出前会调；core 这边没有要收尾的东西
    server.route("POST", "/group/close", [](const std::string &) { return std::string(R"({"ok":true})"); });

    server.route("GET", "/statusline", [&ctx](const std::string &) { return statusline_payload(ctx).dump(); });

    // 引导页预填现值：配置树原样给，客户端改完原样写回
    server.route("GET", "/setup", [&cfg](const std::string &) { return cfg.to_json().dump(); });
    server.route("POST", "/setup", [&cfg](const std::string &body) {
        // 只写引导页那五项：客户端的请求体里还带着 client_id 这类东西
        const json in = parse_body(body);
        for (const char *k : {"protocol", "base_url", "api_key", "model", "small_model"})
            if (in.contains(k) && !cfg.persist(k, in[k]))
                return command_error("写 ~/.realagent/settings.json 失败（原因见 core 日志）");
        cfg.persist("setup_done", true);
        return std::string(R"({"ok":true})");
    });
    server.route("POST", "/setup/models", [](const std::string &body) {
        const auto models = setup_models(parse_body(body));
        if (!models) return command_error(models.error());
        return json{{"ok", true}, {"data", *models}}.dump();
    });

    // 状态栏变了就推一帧。配置只在请求里改（/model），所以每条请求之后比一次就够
    std::string last_statusline = statusline_payload(ctx).dump();
    server.after_request([&] {
        if (std::string cur = statusline_payload(ctx).dump(); cur != last_statusline)
        {
            last_statusline = std::move(cur);
            server.push_event("statusline", last_statusline);
        }
    });

    return server.run(12345) ? 0 : 1;
}
