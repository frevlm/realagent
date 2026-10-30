# ADR-0031：对外暴露 MCP —— 一个 stdio 桥，是 core 的第三个客户端

- 状态：已采纳（2026-10-29）
- 依赖：ADR-0021、ADR-0023、ADR-0029

## 决策

1. **桥，不是 core 里的 MCP server。** `realagent-mcp`（`tui/cmd/realagent-mcp/`）由调用方按 stdio
   拉起，翻译成 core 现有的 HTTP + WebSocket——与 TUI、gui 并列的第三个客户端，core 不多一个端点。
   core 本来就异步；一个桥一个组（ADR-0021），调用方退出组就跟着收；两个协议纪元交给官方 Go SDK。
   连不上 core 就把它拉起来。
2. **异步：`send` / `wait` / `cancel`。** 形状照 Devin 的 create / gather。
   `send` 立刻回 `session_id`，带旧 id 就是接着说；`wait` 等列出的**全部**停下或超时（默认 50 秒，
   超时不是错，没停的回 `running`）；`cancel` 就是 `POST /interrupt`。
   停下时另推一条 `notifications/claude/channel`（Claude Code channels，research preview），
   认得的客户端不必 `wait`。
3. **审批先全放行**，以后改成转给调用方问，只改桥。
4. **真相只在 core。** 停没停问 `GET /sessions`，正文读 `GET /session`。桥只记对话的 workdir，
   以及盘上没有的端点报错原文。为此 core 补两处（都是 ADR-0029「对话连同派生的是一件事」）：
   - `running` 算整段对话：同 `root` 下任何一个在跑或收件箱非空。
   - `finish_run` 先通知邻居、再落 `running_`、最后发 `agent_end`：收到任何一个 `agent_end`
     再问，问到的就是真相。反过来排，最后一帧之后问到的仍是 `running`，再也等不到下一帧。

## 后果

- TUI 会话清单里，子 agent 还在跑时整段对话显示 `running`。
- 子 agent 的 `agent_end` 可能晚于被它叫醒的父 agent 的 `agent_start`；客户端按 `session_id` 各管各的。
