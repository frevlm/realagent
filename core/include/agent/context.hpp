/*
 * context.hpp — agent 够得着的外部东西：配置、模型表、事件出口、MCP 连接池
 *
 * 配置、模型表、连接池是进程级的；事件出口与 online 是一组的——每个 agent 拿一份拷贝，
 * 帧只推给自己那一组的客户端，审批只问自己那一组的客户端（ADR-0021）。
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
    /* 此刻有没有客户端能裁决审批；不设当作有。 */
    std::function<bool()> online;
};

} // namespace realagent
