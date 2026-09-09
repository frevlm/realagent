/*
 * main.cpp — realagent core 入口（常驻服务）
 *
 * 验证模式：
 *   realagent-core test-tools → 工具执行链路验证（read/edit/bash + 权限）
 * 常驻模式（默认）：
 *   realagent-core → 启动 QUIC/HTTP3 服务（PROTOCOL.md 端点）
 *   POST /message → 启动 agent loop
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "agent/agent.hpp"
#include "agent/agents.hpp"
#include "agent/command.hpp"
#include "agent/context.hpp"
#include "agent/executor.hpp"
#include "agent/session.hpp"
#include "config.hpp"
#include "mcp/mcp.hpp"
#include "server/quic_server.hpp"
#include "tools/tools.hpp"

using namespace realagent;

static int run_tool_test(CoreContext &ctx)
{
    ApprovalCoordinator approval; // 测试模式无客户端，ASK 会 30s 超时按 deny
    Executor exe(ctx, approval, std::filesystem::current_path().string());

    // 上一次跑剩的文件先删掉：留着它 edit(create) 就走成了 append 分支，
    // 这条验证从第二次运行起就不再验证它声称验证的东西
    std::remove("/tmp/ra_edit_test.txt");

    const auto show = [](const char *tag, const nlohmann::json &r) {
        printf("%s: status=%d output=%.80s\n", tag, r["status"].get<int>(),
               r["output"].get<std::string>().c_str());
    };
    show("read", exe.execute("t1", "read", "{\"file_path\":\"" __FILE__ "\"}"));
    show("create", exe.execute("t2", "edit", R"({"edits":[{"file_path":"/tmp/ra_edit_test.txt","new_text":"hello realagent\nline2"}]})"));
    // 行号与 hash 就抄 read 印出来的那两个值——跟模型走的是同一条路
    const nlohmann::json r = exe.execute("t1", "read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}");
    show("read", r);
    const std::string h1 = r["output"].get<std::string>().substr(2, 3);
    show("replace", exe.execute("t2", "edit", nlohmann::json{{"edits", {{{"file_path", "/tmp/ra_edit_test.txt"}, {"line", 1}, {"hash", h1}, {"new_text", "HELLO"}}}}}.dump()));
    show("verify", exe.execute("t1", "read", "{\"file_path\":\"/tmp/ra_edit_test.txt\"}"));
    show("bash", exe.execute("t3", "bash", "{\"command\":\"echo tools-ok && ls /tmp/ra_edit_test.txt\"}"));
    return 0;
}

int main(int argc, char **argv)
{
    // 文件坏了就地退出：读不懂用户写了什么，就别带着半份配置往下跑
    auto loaded = Config::load();
    if (!loaded)
    {
        fprintf(stderr, "[config] %s\n", loaded.error().c_str());
        return 1;
    }
    auto cfg = std::move(*loaded); // 非 const：persist 需写路径
    CoreContext ctx;
    ctx.config = &cfg;

    // 模型数据表读一次，进程级只读（ADR-0010 不热重载；ADR-0019 不进 Agent）。
    // 读不动就报出来，之后照常跑——没有表只是不算钱，不是"不能对话"，
    // 为它拒绝启动是把一个次要功能提成了必需品
    std::string price_err;
    const Pricing pricing = Pricing::load(cfg, &price_err);
    if (!price_err.empty()) fprintf(stderr, "[llm] %s（本次运行不计价）\n", price_err.c_str());
    ctx.pricing = &pricing;

    /* MCP 连接池（ADR-0023）：进程级，一份配置一个连接。真正连上是在建 agent 那一刻
     * （那时才知道 workdir，项目级 mcp.json 从它算起）。 */
    McpHub mcp_hub;
    ctx.mcp = &mcp_hub;

    /* 端点那一束（protocol / base_url / model）没有默认值，缺了就跑不了（ADR-0017）。
     *
     * 但**不在这里退出**：core 是常驻服务，TUI 是另一个进程。core 一退，
     * 用户在 TUI 里看见的是一句"连不上"——比配错了还难诊断，
     * 恰好犯了"报错了用户啥都不知道"这条本身要治的病。
     * 所以照常起、启动日志喊一遍，同时把这段话原样交给任何一次 POST /message，
     * 让它出现在用户正盯着的那块屏幕上。 */
    const std::string cfg_error = endpoint_config_error(cfg);
    if (!cfg_error.empty()) fprintf(stderr, "[config] %s\n", cfg_error.c_str());

    // 线程安全事件队列：agent 线程 emit → 事件循环 on_tick flush 到推送流
    // （ADR-0002 线程模型：quiche 非线程安全，推送必须在事件循环线程）
    std::mutex ev_mtx;
    std::deque<std::pair<std::string, std::string>> ev_queue;
    ctx.emit_fn = [&ev_mtx, &ev_queue](const std::string &type, const std::string &payload) {
        std::lock_guard<std::mutex> lk(ev_mtx);
        ev_queue.emplace_back(type, payload);
    };

    for (const auto &t : tool_defs())
        fprintf(stderr, "  tool: %s (dangerous=%d)\n", t.value("name", std::string()).c_str(),
                (int)tool_dangerous(t));

    ApprovalCoordinator approval;
    approval.set_emit(ctx.emit_fn); // permission_request 也走队列

    if (argc > 1 && std::string(argv[1]) == "test-tools")
    {
        return run_tool_test(ctx);
    }

    // —— 常驻服务模式 ——
    // core 启动时 agent 数为 0，不自动创建任何 agent（ADR-0019）：自动建就得替用户
    // 猜 workdir，而 ADR-0017 刚把猜赶出去。客户端连上来自己建第一个。
    Agents pool(ctx, approval);

    QuicServerConfig scfg;
    // 证书用全局绝对路径（不依赖 cwd）
    {
        const std::string home = getenv_or("HOME", ".");
        scfg.cert_file = home + "/.realagent/cert.pem";
        scfg.key_file = home + "/.realagent/key.pem";
    }
    QuicServer server(scfg);
    // 没有客户端订阅推送流 = 没人能裁决危险工具，当场拒绝（ADR-0019 §8）
    approval.set_online([&server] { return server.has_client(); });

    QuicCallbacks cbs;
    // 斜杠命令列表（GET /commands，TUI 菜单数据源）
    cbs.on_commands = [&pool](const std::string &body) {
        // agent_id 可选：prompt 命令表跟着那个 agent 的 workdir 走，不带就只有内置那三条
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.is_object() ? j.value("agent_id", 0) : 0;
        return command_defs(id > 0 ? pool.find(id) : nullptr).dump();
    };

    // POST /agent：建一个 agent。workdir 必传，core 不猜
    cbs.on_agent = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::string err;
        const int id = pool.create(j.value("workdir", ""), 0, {}, {}, err);
        if (id <= 0) return command_error(err);
        return nlohmann::json{{"ok", true}, {"agent_id", id}}.dump();
    };
    // GET /agents：列出所有 agent
    cbs.on_agents = [&pool](const std::string &) {
        return pool.list().dump();
    };

    /* GET /session（兼别名 GET /history）：一个 agent 的会话内容回放成事件帧（ADR-0020）。 */
    cbs.on_session_get = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return command_error("无此 agent: " + std::to_string(id));
        return Session::read_frames(a->session_dir(), a->session_id()).dump();
    };

    cbs.on_message = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const std::string user_input = j.value("message", "");
        if (user_input.empty()) return command_error("empty message");
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return command_error("无此 agent: " + std::to_string(id));
        if (user_input[0] == '/') return handle_command(ctx, pool, *a, user_input);
        if (!cfg_error.empty()) return command_error(cfg_error);
        a->post(user_input);
        return std::string("{\"status\":\"processing\"}");
    };
    cbs.on_command = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        std::string cmd = j.value("command", "");
        if (cmd.empty()) return command_error("empty command");
        if (cmd[0] != '/') cmd.insert(cmd.begin(), '/');
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return command_error("无此 agent: " + std::to_string(id));
        return handle_command(ctx, pool, *a, cmd);
    };
    // GET /sessions 与 POST /session 都要指名道姓：会话目录跟着 agent 的 workdir 走
    cbs.on_sessions = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return command_error("无此 agent: " + std::to_string(id));
        auto lk = a->try_lock();
        if (!lk.owns_lock()) return command_error(AGENT_BUSY);
        return sessions_payload(pool, *a).dump();
    };
    cbs.on_session = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        const int id = j.value("agent_id", 0);
        Agent *a = pool.find(id);
        if (!a) return command_error("无此 agent: " + std::to_string(id));
        auto lk = a->try_lock();
        if (!lk.owns_lock()) return command_error(AGENT_BUSY);
        const std::string sid = j.value("id", "");
        if (sid.empty())
            a->reset();
        else if (!a->resume(sid))
            return command_error("unknown session: " + sid);
        return nlohmann::json{{"ok", true},
                              {"data", sessions_payload(pool, *a)}}
            .dump();
    };
    cbs.on_interrupt = [&](const std::string &body) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        Agent *a = pool.find(j.value("agent_id", 0));
        if (!a) return;
        a->interrupt();
        approval.cancel(a->id());
    };

    // 客户端正常退出
    cbs.on_group_close = [](const std::string &) {
        return std::string("{\"ok\":true}");
    };

    cbs.on_approval_response = [&approval](const std::string &id, bool allow) {
        approval.respond(id, allow);
    };
    // 状态栏数据（GET /statusline）：客户端启动时拉一次，之后由 statusline 帧推更新
    cbs.on_statusline = [&ctx]() { return statusline_payload(ctx).dump(); };
    // 启动值取一次，避免首轮推一帧与客户端 GET /statusline 重复的内容
    std::string last_statusline = statusline_payload(ctx).dump();
    cbs.on_tick = [&]() {
        if (std::string cur = statusline_payload(ctx).dump(); cur != last_statusline)
        {
            last_statusline = std::move(cur);
            server.push_event("statusline", last_statusline);
        }

        std::deque<std::pair<std::string, std::string>> batch;
        {
            std::lock_guard<std::mutex> lk(ev_mtx);
            batch.swap(ev_queue);
        }
        for (auto &[t, p] : batch) server.push_event(t, p);
    };

    server.set_callbacks(cbs);
    server.run(); // 阻塞事件循环

    return 0;
}
