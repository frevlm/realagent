/*
 * context.hpp — 进程级的共享东西：配置、模型表、事件出口、MCP 连接池
 */
#pragma once

#include <functional>
#include <string>

#include "config.hpp"

namespace realagent {

/* 事件出口：type + JSON 载荷文本。main 挂上的是入队函数，事件循环线程负责推送。 */
using EmitFn = std::function<void(const std::string &type, const std::string &payload)>;

class Pricing;
class McpHub;

struct CoreContext {
    Config *config = nullptr;
    const Pricing *pricing = nullptr;
    EmitFn emit_fn;
    McpHub *mcp = nullptr;
};

} // namespace realagent
