/*
 * mcp_stub_server.cpp — 一个说 2026-07-28 的假 MCP server，只服务测试
 *
 * 官方参考 server（@modelcontextprotocol/server-everything）实测是旧纪元的：
 * `server/discover` 回 -32601，只认 `initialize`。而我们只说新纪元（ADR-0023 §5），
 * 所以现成的对手一个都用不了——这个 stub 就是那个对手。
 *
 * 它刻意复现两个真实踩过的坑：
 *   1. **在 tools/list 的响应之前先吐一条通知**。响应和通知共用同一条 stdout，
 *      "写一行读一行"的客户端会把通知当成答案
 *   2. **分页**：工具分两页给，不跟 nextCursor 就只看得见第一页
 *
 * argv[1] 可以给一个模式名，让同一个二进制扮演别的失败：
 *   badversion —— 一律回 -32022，data.supported 列出它说得了什么
 *   slow       —— 收到 tools/call 不回，用来验超时与 notifications/cancelled
 *   weirdnames —— 名字带空格和括号，照抄官方 Go SDK 的 everything 示例
 */
#include <chrono>

#include <iostream>
#include <string>
#include <thread>

#include <realetting/json.hpp>

using nlohmann::json;

static void emit(const json &j)
{
    // 一行一条，行内不得有换行——这是 stdio 传输的全部帧格式
    std::cout << j.dump() << "\n"
              << std::flush;
}

static json tool(const std::string &name, const std::string &desc)
{
    json t;
    t["name"] = name;
    t["title"] = "Title of " + name; // core 不该收下它
    t["description"] = desc;
    // 真 server 就是这么发的：官方参考 server 每个工具的 inputSchema 都带 draft-07 的 $schema
    t["inputSchema"] = {{"$schema", "http://json-schema.org/draft-07/schema#"},
                        {"type", "object"},
                        {"properties", json::object()}};
    t["annotations"] = {{"readOnlyHint", true}, {"destructiveHint", false}};
    return t;
}

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";
    std::string line;
    while (std::getline(std::cin, line))
    {
        json msg = json::parse(line, nullptr, false);
        if (msg.is_discarded()) continue;
        const auto id = msg.find("id");
        const std::string method = msg.value("method", std::string());
        if (id == msg.end()) continue; // 通知：收下，不回

        // 每个请求都必须自带版本与能力，缺了按规范是 -32602
        const json params = msg.value("params", json::object());
        const json meta = params.value("_meta", json::object());
        if (!meta.contains("io.modelcontextprotocol/protocolVersion") ||
            !meta.contains("io.modelcontextprotocol/clientCapabilities"))
        {
            emit({{"jsonrpc", "2.0"},
                  {"id", *id},
                  {"error", {{"code", -32602}, {"message", "missing required _meta fields"}}}});
            continue;
        }

        if (mode == "badversion")
        {
            emit({{"jsonrpc", "2.0"},
                  {"id", *id},
                  {"error",
                   {{"code", -32022},
                    {"message", "Unsupported protocol version"},
                    {"data", {{"supported", json::array({"2025-11-25"})}, {"requested", meta["io.modelcontextprotocol/protocolVersion"]}}}}}});
            continue;
        }

        if (method == "tools/list" && mode == "weirdnames")
        {
            json res;
            res["resultType"] = "complete";
            // 三个都是 Go SDK 的 everything 示例里真实存在的名字
            res["tools"] = json::array({tool("greet", "ok"), tool("greet (with Icons)", "spaces"),
                                        tool("elicit (form)", "spaces")});
            emit({{"jsonrpc", "2.0"}, {"id", *id}, {"result", res}});
            continue;
        }

        if (method == "tools/list")
        {
            // 坑 1：先插一条通知。没订阅也照发——server 想发就发，客户端得能不当回事
            emit({{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}});
            const std::string cursor = params.value("cursor", std::string());
            json res;
            res["resultType"] = "complete";
            if (cursor.empty())
            {
                res["tools"] = json::array({tool("echo", "Echo the input back.")});
                res["nextCursor"] = "page2"; // 坑 2：还有一页
            }
            else
            {
                res["tools"] = json::array({tool("get-tiny-image", "Return an image block.")});
            }
            emit({{"jsonrpc", "2.0"}, {"id", *id}, {"result", res}});
            continue;
        }

        if (method == "tools/call")
        {
            if (mode == "slow")
            {
                std::this_thread::sleep_for(std::chrono::seconds(60)); // 不回，等客户端放弃
                continue;
            }
            const std::string name = params.value("name", std::string());
            json res;
            res["resultType"] = "complete";
            if (name == "echo")
            {
                const json args = params.value("arguments", json::object());
                res["content"] = json::array(
                    {{{"type", "text"}, {"text", "Echo: " + args.value("message", std::string())}}});
                res["isError"] = false;
            }
            else if (name == "get-tiny-image")
            {
                // 文本 + 图片：正是我们要能原样端到 upstream 那一层的形状
                res["content"] = json::array({{{"type", "text"}, {"text", "here it is:"}},
                                              {{"type", "image"},
                                               {"data", "iVBORw0KGgo="},
                                               {"mimeType", "image/png"}}});
                res["isError"] = false;
            }
            else
            {
                // 参数/工具不对：真 server 也是把它降级成工具错误而不是 JSON-RPC 错误
                res["content"] =
                    json::array({{{"type", "text"}, {"text", "no such tool: " + name}}});
                res["isError"] = true;
            }
            emit({{"jsonrpc", "2.0"}, {"id", *id}, {"result", res}});
            continue;
        }

        emit({{"jsonrpc", "2.0"},
              {"id", *id},
              {"error", {{"code", -32601}, {"message", "Method not found: " + method}}}});
    }
    return 0;
}
