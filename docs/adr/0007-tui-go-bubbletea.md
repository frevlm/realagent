# ADR-0007: TUI 采用 Go + Bubble Tea

- 状态：已接受
- 日期：2026-08-09

## 决策

TUI（独立客户端进程）采用 **Go + Bubble Tea**（Charmbracelet TUI 框架）。core 仍为 C++（QUIC/HTTP3 服务，ADR-0006），两者通过 QUIC/HTTP3 + JSON 通信（PROTOCOL.md）。

## 背景

QUIC 服务架构（ADR-0006）使 TUI 语言不受 C++ 约束。用户："因为引入了http连接, tui似乎可以不用C++写了, 如果有更好的语言可以向我提出"。

## 调研对标

| 项目 | TUI 技术 | 验证 |
|---|---|---|
| **OpenCode / Crush**（Charmbracelet） | **Go + Bubble Tea**——Model/Update/View 模式，组件树、模态弹窗、页面路由 | TUI 场景 Go + Bubble Tea 的成熟范例 |
| **Codex（Rust 侧）** | Ratatui + React 式组件模型（app.rs / chatwidget.rs，模块 <500 行策略，insta snapshot 测试） | Rust 可行但开发慢 |
| **Pi** | 自研 TS TUI（pi-tui 包，差分渲染） | TS 自研成本高 |
| **Claude Code** | TypeScript 自研 TUI | 终端渲染性能一般 |

## 候选方案与权衡

1. **Go + Bubble Tea（选定）** —— TUI 生态事实标准（OpenCode/Crush 验证）。goroutine + channel 与事件流天然匹配，开发迭代快，编译为单一静态二进制分发简单。缺点：GC 语言，性能不如 Rust（TUI 场景无所谓）。
2. **Rust + Ratatui** —— Codex 方案，性能与类型安全最佳，但开发慢，事件流需引入 tokio 异步运行时，复杂度高。
3. **TypeScript + Ink/自研** —— React 心智模型，终端渲染性能一般，分发依赖 node/bun。
4. **Python + Textual** —— 上手快但终端性能最差，不适合 agent 的高动态消息流。

选定 Go。

## UI 形态

参考 **claude code / codex 客户端**外观，**无状态栏**（用户明确）。核心界面元素：消息流（LLM 回复 + 工具执行展示）+ 底部输入框 + 审批对话框。流式打字效果由推送流帧到达驱动（全可靠，见 PROTOCOL.md）。

> **实况注（2026-08-16）：「无状态栏」这条已被推翻，状态栏是现实。**
> 输入框下方有常驻状态栏（`🤖 model | 📁 dir | 🌿 git`），core 侧有 `GET /statusline` 端点与 `statusline` 推送帧，TUI 侧有 statusline.go 与 `/statusline` 配置命令，CONTEXT.md 也有完整的 [[Statusline]] 词条。它不是顺手加的，是一条被完整设计过的特性。
> 推翻发生在何时、因何推翻，**没有任何记录**——本 ADR 没改，也没有后继 ADR 声明取代它。这条注只陈述事实，不追补理由：编一个当时的理由出来，比留着这个缺口更糟。
> 另外状态栏与**状态行**（读秒行，`status_update` 帧）是两个东西，见 PROTOCOL.md。

> **实况注（2026-11-03）：上表 OpenCode 那一行已经过时。** OpenCode 1.0 把 Go + Bubble Tea 的 TUI 换成了自研的 OpenTUI（Zig + SolidJS），release notes 说旧 TUI "had performance and capability issues"。
> 本项目仍选 Go：实测到的毛病（光标不在输入处、帧不是一次画完）都出在 Bubble Tea v1 的渲染器上，v2 已经解决；Charm 自己的 Crush 是 Go + v2。迁移与布局的改动见 ADR-0033。

## 后果

- 项目变为**双语言**：C++（core 服务）+ Go（TUI 客户端），边界是 QUIC/HTTP3 + JSON 协议（PROTOCOL.md）。
- TUI 的职责纯粹：渲染 + 用户输入 + QUIC 客户端；一切 agent 逻辑在 core。
- 构建编排需同时处理 CMake（core）+ Go module（TUI），CMake 统一入口调 go build。
- 事件流订阅（推送流）是 Go 侧与 core 的核心交互面。
- Bubble Tea 事件模型与 QUIC 异步事件流的桥接（异步事件 → tea.Msg）为 M6 实现风险（PLAN.md R15）。
