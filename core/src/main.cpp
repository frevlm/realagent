/*
 * main.cpp — realagent core 入口：读配置、建 agent 池、挂路由、监听
 */
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

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
    // 一组 agent 的事件只推给那个客户端，审批也只问它（ADR-0021）
    Agents pool(
        [&ctx, &server](const std::string &client) {
            CoreContext c = ctx;
            c.emit_fn = [&server, client](const std::string &type, const std::string &payload) {
                server.push_to(client, type, payload);
            };
            c.online = [&server, client] { return server.has_client(client); };
            return c;
        },
        approval);

    /* 一条消息（或展开好的 prompt 命令）发进一段对话。agent 在这里才建出来，
     * 所以没有从没说过话的 agent（ADR-0029）。 */
    const auto submit = [&](const json &j, const std::string &text) {
        // 每条现算：POST /setup 会在运行中把端点配齐
        if (std::string e = endpoint_config_error(cfg); !e.empty()) return command_error(e);
        std::string err;
        const auto a = pool.open(j.value("client_id", ""), j.value("workdir", ""), j.value("session_id", ""), err);
        if (!a) return command_error(err);
        a->post(text);
        return std::string(R"({"status":"processing"})");
    };
    const auto command = [&](const json &j, const std::string &input) {
        const auto a = pool.find(j.value("client_id", ""), j.value("session_id", ""));
        const CommandOutcome o = run_command(ctx, j.value("workdir", ""), a.get(), input);
        return o.reply.empty() ? submit(j, o.post) : o.reply;
    };

    server.route("POST", "/message", [&](const std::string &body) {
        const json j = parse_body(body);
        const std::string msg = j.value("message", "");
        if (msg.empty()) return command_error("empty message");
        return msg[0] == '/' ? command(j, msg) : submit(j, msg);
    });
    server.route("POST", "/command", [&](const std::string &body) {
        const json j = parse_body(body);
        std::string cmd = j.value("command", "");
        if (cmd.empty()) return command_error("empty command");
        if (cmd[0] != '/') cmd.insert(cmd.begin(), '/');
        return command(j, cmd);
    });
    server.route("GET", "/commands", [](const std::string &body) {
        return command_defs(parse_body(body).value("workdir", "")).dump();
    });

    // 读的是盘上那份，不需要 agent 在场。派生的 agent 落在 sub/，嵌在父对话里看时也从这里取
    const Handler session_get = [](const std::string &body) {
        const json j = parse_body(body);
        const std::string dir = sessions_dir(j.value("workdir", "")), id = j.value("session_id", "");
        return (Session::exists(dir, id) ? Session::read_frames(dir, id) : Session::read_frames(dir + "/sub", id)).dump();
    };
    server.route("GET", "/session", session_get);
    server.route("GET", "/history", session_get);
    server.route("GET", "/sessions", [&pool](const std::string &body) {
        const json j = parse_body(body);
        return sessions_payload(pool, j.value("client_id", ""), j.value("workdir", "")).dump();
    });

    server.route("POST", "/interrupt", [&pool](const std::string &body) {
        const json j = parse_body(body);
        pool.interrupt(j.value("client_id", ""), j.value("session_id", ""));
        return std::string(R"({"status":"ok"})");
    });
    server.route("POST", "/approval-response", [&approval](const std::string &body) {
        const json j = parse_body(body);
        approval.respond(j.value("id", ""), j.value("allow", false));
        return std::string(R"({"status":"ok"})");
    });
    server.route("POST", "/group/close", [&pool](const std::string &body) {
        pool.close_group(parse_body(body).value("client_id", ""));
        return std::string(R"({"ok":true})");
    });

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

    // 断线满 60 秒还没回来就关组（ADR-0021 §3）。60 是选定的数，不是推导出来的
    std::jthread reaper([&server, &pool](std::stop_token st) {
        std::mutex m;
        std::condition_variable_any cv;
        std::unique_lock<std::mutex> lk(m);
        while (!cv.wait_for(lk, st, std::chrono::seconds(5), [] { return false; }) && !st.stop_requested())
            for (const std::string &c : server.gone_for(std::chrono::seconds(60))) pool.close_group(c);
    });

    return server.run(12345) ? 0 : 1;
}
