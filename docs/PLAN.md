# 实现规划（Phase 1）

> 里程碑拆解 + 技术风险清单 + 调研发现。规划中发现的问题在此暴露，逐项拷问后回填。
> 架构决策见 CONTEXT.md 与 docs/adr/（0001-0016；实况注：现已到 0025）。
>
> **实况注（2026-08-25）：M1「插件加载链」整条已作废**（[[ADR-0016]]）。插件系统于本日废除，
> 5 个容器并入 core。M2 / M4 里凡以"插件"为形态的交付物，交付的能力都还在，只是不再隔着
> C ABI——协议与计价在 `core/src/llm/llm.cpp`，工具在 `core/src/tools/tools.cpp`，
> 权限是一个配置键。本文按惯例**不改写历史**，只在各处补注。

## 里程碑总览

```
M0 骨架与构建链      → M1 插件加载链   → M2 工具注册执行
M3 Agent Loop/事件流 → M4 Provider 插件 → M5 QUIC 通信
M6 TUI              → M7 集成与测试
```

依赖关系：M0 → M1 → M2 → M3 → M4 → M5 → M6，M7 收尾。M3/M4 可部分并行（事件流引擎与 Provider 插件相对独立，但 M4 的响应解析要喂给 M3 的事件流）。

## 调研背景速览（详细调研见各 ADR 与 OPENCODE_RESEARCH.md）

| 项目 | 语言 | 核心参考点 |
|---|---|---|
| Pi（earendil-works/pi） | TypeScript | 极简核心 + 插件架构；agent 事件流；ExtensionAPI；JSONL 会话树 |
| OpenCode（已归档） | Go | Bubble Tea TUI；Provider 抽象；SQLite；pub/sub 事件 |
| Codex（openai/codex） | Rust/TS | app-server 架构；沙箱执行；patch 系统；skills/plugins 分层扩展 |
| Goose（block/goose） | Rust | MCP 扩展 + 自定义发行版；V8 内嵌 |

## M0：骨架与构建链

**交付物**
- 目录结构落地（CONTEXT.md 中的布局：core/ tui/ plugins/ docs/）
- 双语言构建编排：CMake 统一入口，自定义 target 调 `go build`（TUI），core + plugins 走原生 CMake
- C++26 协程可行性 spike（Clang 19+ / brew llvm）
- JSON 库接入（nlohmann/json 单头 vendored）、spdlog 接入
- 插件 SDK 头文件 v0：plugin.json 结构、类型枚举、生命周期接口初版

**已定实现约定**
- 测试框架：C++ 用 Catch2（单头集成），Go 用内置 testing
- API key：`.realagent/settings.json` 的 `api_key`（不读 env；core 默认树里是空串，缺省即不发 Authorization）

**技术要点**
- JSON：nlohmann/json 3.12.0 单头文件，逐字节上游，vendored 在 `core/include/json.hpp`。**core 不再包一层壳**——链式 `a["b"]["c"]` 与隐式转换（`std::string s = j["k"];`）是它自带的。读不受控的输入（HTTP 体、SSE 帧、盘上的文件）用 `find()` / `value(key, 默认值)`：const `operator[]` 撞上缺键是未定义行为。解析用 `parse(text, nullptr, false)` + `is_discarded()`，不抛。
- Clang C++26 协程：C++20 协程核心完全支持（P0912R5，macOS 全支持）；P2561 非抛出协程未实现但本项目不依赖（见 ADR-0003）。

**风险**（每条分「决策」与「实现」两行，两者互不蕴含）
- **R1: Clang 19+ 的 C++26 协程支持度**
  - 决策：已解除（2026-08-09）——Clang 对 C++20 协程核心完全支持；P2561 未实现但本项目不依赖。ADR-0003 已修正。
  - 实现：风险解除类，无实现物。附注：core 至今一行协程都没有（全仓无 `co_await`/`co_return`），LLM 流式请求走独立线程 + 阻塞 libcurl（core/src/main.cpp:382 起线程，core/src/agent/agent.cpp:173 起 curl）。
- **R2: msquic 与 C++/CMake 集成**
  - 决策：已废弃——不用 msquic（纯传输层无 H3 语义）。
  - 实现：风险废弃类，无实现物。**但替代品与本文原记载不符**：实际选用的是 Cloudflare quiche（见 R12）。

## M1：插件加载链

> **实况注（2026-08-25）：本里程碑的交付物已整体删除**（[[ADR-0016]]）。dlopen、ABI 校验、
> 能力表、能力槽、deps DAG、启停级联、`plugin.json`——全部不再存在。它服务过的第三方容器数是 **0**。
> 下方内容保留为设计史：这套机制是对的，只是没有用户。

**交付物**
- plugin.json 解析（名称/描述/版本/ABI 版本/前置依赖）——**无 type**（ADR-0011）
- dlopen 加载 + ABI 版本强校验（PI_ABI_VERSION，不等则拒绝并提示重编）
- 能力槽：能力由非空函数指针决定（协议槽 / 权限槽），槽位独占，冲突即点名双方（ADR-0011）
- 前置依赖声明解析（加载序）+ init 中显式 `claim` 接管内层（ADR-0011）
  - 实况注（2026-08-16）：以上两条的 ADR-0011 机制**均已被 ADR-0012 取代**。①「能力由非空函数指针决定」→ 容器经 `capabilities()` 交出 `{名字, 函数}` 表，core 按名查（`find_capability`）；`plugin_api_t` 固定五项，新增能力不破 ABI。②「槽位冲突点名双方」→ 冲突时**空置该槽并点名**，不再卸载插件（决策 11）。③「`claim` 接管内层」→ **整套接管机制已删除**，改为[[管线]]分段：插件互不认识、互不调用，跨容器取用走 `import`（决策 1/12）。槽位独占与依赖声明解析两条不变。
- perm-allow-all、session-manager 两个插件骨架
  - 实况注（2026-08-16）：`session-manager` 已删除（其 `/new` `/resume` 与 core 内置重复且只是空壳，两命令现由 core 直接处理，core/src/main.cpp:242/246）。实际交付的权限插件是 **perm-allow-all + perm-ask 两个**（二选一占权限槽），perm-ask 从未在本清单里出现过。插件仓现共 5 个容器，见 docs/plugins.md。
  - 实况注（2026-08-25）：整条作废（[[ADR-0016]]）。`docs/plugins.md` 已改为 `docs/capabilities.md`（core 内置能力清单），权限从两个动态库变成配置键 `permission` 的三个值。

**技术要点**
- 插件加载链是第一阶段的**交付关键路径**（core 零内置工具，一切能力来自插件）。
- 插件配置由 core 统一注入（代码默认树打底 < settings.json 覆盖，不读 env），插件初始化接口接收合并后的配置节。
- 事件订阅：单入口 `on_event(handle, event*)`，插件内按 type 字符串分发。

**风险**
- **R3: 插件事件订阅接口**
  - 决策：已定（2026-08-09）——单入口 + 插件内按 type 字符串分发（ADR-0001）。
  - 实现：曾实现为能力键 `event.observe`（扇出在 realugin 的 `PluginManager::emit`）。**2026-08-25 随 [[ADR-0016]] 删除**：没有订阅者了，事件的去处只有客户端——agent 线程 `emit` 入队、事件循环 flush 到推送流，一条路（core/src/main.cpp）。
- **R4: 嵌套组装时机**
  - 决策：已定——加载序静态定，跨容器引用运行时按名取。
  - 实现：曾实施（2026-08-10）——按 `deps` 反图、自入度 0 起 BFS 逐层 init。**2026-08-25 随 [[ADR-0016]] 删除**：没有容器，也就没有加载序。

## M2：工具注册与执行

**交付物**
- 工具注册表（宏 + JSON schema 字符串注册，如 `PI_TOOL_DEFINE("read", "{...}")`）
- 严格顺序执行调度（单 agent 内工具一个接一个）
- 权限检查点（core 钩子 → perm-allow-all）
- core-tools 插件：read / edit / bash（**一个插件注册三个工具**）
- 工具结果 JSON：`{status, messages}` 结构

**技术要点**
- write 不单独存在——write 是 edit 的 `+x-0` 特例（空文件追加 = 创建），LLM 创建文件用 edit。
- edit 工具描述写清「目标文件不存在时创建新文件；old_string 为空表示从空文件/开头追加」——创建语义直接进描述，让模型学会创建文件用 edit。
- bash stdout 作为 tool_output 帧走推送流（实时可见），命令完整输出在 tool_result 帧回传。
- 权限检查点独立于工具实现——同一检查点适用于所有危险工具（write/bash）。

**风险**
- **R5: edit 的 +x-0（write）语义**
  - 决策：已定（2026-08-09）——创建语义写进工具描述。
  - 实现：已实现——描述文案与空 `old_string` 追加分支现在 core/src/tools/tools.cpp（2026-08-25 前在 realagent-plugins/core-tools/core_tools.c）。
- **R6: bash 流式输出**
  - 决策：已定（2026-08-09）——stdout 走 `tool_output` 帧（推送流，全可靠），`tool_result` 帧回传完整输出。
  - 实现：**已实现**（2026-08-16）——帧由工具自己推出（`emit_output`，bash 读循环里逐行调用，现在 core/src/tools/tools.cpp）；TUI 在 `tool_output` 分支按"续写开着的行"渲染（tui/cmd/realagent-tui/main.go），因为超长行会被切成几帧、末段可能无换行符。载荷 `{call_id, stream, text}` 见 docs/PROTOCOL.md。端到端实测：bash 循环输出边跑边到帧。

## M3：Agent Loop + 事件流

**交付物**
- 异步事件流引擎：agent_start / turn_start / message_* / tool_execution_* / turn_end / agent_end
  - 实况注（2026-08-16）：这套里有三个**至今零 emit 点**——`agent_start`、`agent_end`、`message_end`。`message_start` 也只在收到用户输入时发一帧 `{"role":"user"}`，assistant 消息不发。即 agent 与 message 两级生命周期实际都没有闭合，客户端只能靠 `turn_end` 判收工。逐帧实现状态见 docs/PROTOCOL.md 帧表。
- 单 agent，工具严格顺序
- 中止（abort）信号
- 会话状态 + JSONL 读写（新建/恢复/列表，无 fork）
  - 实况注（2026-08-16）：**已实现**——`Session`（core/include/agent/session.hpp + core/src/agent/session.cpp）。新建 `/new`、恢复 `/resume <id>`、列表 `/resume`，另有 `GET /sessions` / `POST /session` 两个端点。fork 与会话树仍不做（本就是第一版范围外）。见 R9。

**技术要点**
- **Loop 形态**：`while(1)`——调 LLM → 逐个收事件 → 产生事件时广播给订阅者列表（fan-out 到推送流/插件/日志）→ 顺序执行工具 → 结果回传 → 直到 LLM 完成。协程仅用于 LLM 流式请求的异步等待。
- 会话 JSONL schema：每行一个 JSON 对象，append-only；type 区分 user / assistant / tool_call / tool_result；call_id 关联工具调用与结果；含 id / ts / content / model 字段。
- Steering：第一版只支持中止（abort），不支持中途插话。

**风险**
- **R7: 事件流实现形态**
  - 决策：已定（2026-08-09）——while(1) + 事件广播 fan-out（ADR-0002）。
  - 实现：已实现——`Agent::run` 的 turn 循环在 core/src/agent/agent.cpp，`broadcast` 经 `emit_fn` 入事件队列，事件循环 flush 到推送流（2026-08-25 起只此一个去处，见 R3）。
- **R8: 中止传播**
  - 决策：已定——LLM 流式中断 + 工具执行中断共用一个信号。
  - 实现：**已实现**（2026-08-16），决策要的「一个信号同时停住两侧」完整落地。
    - LLM 侧：`abort_` 原子量（core/include/agent/agent.hpp）、curl `XFERINFOFUNCTION` 回调、`Agent::run` 循环里的四个检查点、`POST /interrupt`（core/src/server/quic_server.cpp → core/src/main.cpp 的 `on_interrupt` 调 `agent.interrupt()` + `approval.cancel_all()`）。
    - 工具侧：`Agent::interrupt()` 顺手调 `Executor::interrupt()`（core/src/agent/executor.cpp）。executor 记下"手上有没有在跑的"（登记在先、执行在后，杜绝"检查完了才开始跑"的缝），中止时调 `interrupt_tool()`，打的是 bash 子进程**进程组**（core/src/tools/tools.cpp）：首次 SIGTERM，再按一次 SIGKILL，命令拉起的整棵子孙树一起收掉。
      - 实况注（2026-08-25）：从前这里要按 call_id 找到"在跑的那个容器"再调它的可选能力 `tool.interrupt`，不提供该能力的容器就不可中断。[[ADR-0016]] 之后只有 bash 会跑很久，一个 pid 就记得住。
    - **不具备该能力 = 不可中断**，core 照实等它跑完，不假装成功。这一条是刻意的：假报成功会让上层以为后台已经干净。
    - 「算不算被中断」由 core 判（是 core 提的），结果经 `tool_execution_end` 的 `interrupted` 字段与 `tool_result` 的 `[interrupted by user]` 一并交代——模型对"被打断"与"命令失败"的反应完全不同。
    - 端到端实测：bash 死循环执行中 `POST /interrupt` → 子进程组被收、`status=143`、`interrupted=true`、`interrupted` 帧到达，无孤儿进程残留。
    - 实况注（2026-10-06）：「一个 pid 就记得住」在多 agent 之后不成立——各 agent 的线程会同时跑 bash，全局 pid 互相覆盖，中断 A 可能杀掉 B 的命令。现已改为每个 Executor 一个中断标志，bash 与 hook 共用 `core/src/proc.cpp` 的 `run_proc`（pid 是局部的，轮询标志后 SIGTERM，1 秒后 SIGKILL）。`interrupt_tool()` 与全局 pid 已删除；`test_tools` 里有一条两个 agent 并发跑 bash、只中断其一的用例。
- **R9: JSONL 会话 schema**
  - 决策：已定（2026-08-09）——append-only，一行一条，call_id 关联工具调用与结果。
    **落地时改了 schema 形状**（2026-08-16）：原记「类型区分 user/assistant/tool_call/tool_result + 外层信封（id/ts/content/model）」，实际存的是**抽象对话里的那一条消息，原样**（`{"role":..., "content":[...]}`）。
    改的理由：原 schema 是抽象对话形状定下来之前的设计，两者不同形就得写一对转换函数，而转换函数正是丢字段的地方（thinking 的 `signature`、一条 assistant 消息里的多个 `tool_use`、将来任何新块类型）；同形则恢复即读取，没有转换、没有版本漂移。`call_id` 关联本来就在块里（`tool_use.id` / `tool_result.tool_use_id`），信封是多余的一层。append-only 与"人可读可 diff"两条决策不变。
  - 实现：**已实现**（2026-08-16）。
    - 落盘：`.realagent/sessions/<id>.jsonl`，id 形如 `20260816-153739-31d3`（时间戳 + 4 位随机，字典序 = 时间序，所以清单不需要索引文件）。相对 cwd——会话按项目分家。
    - 入账唯一入口 `Agent::record()`：进内存的同时落盘。`Agent::run` 里六处产生消息全走它，散着写 `push_back` 迟早漏一处，而漏掉的那条恢复时就凭空消失了。
    - 清单元数据不另存：条数 = 行数，标题 = 第一条 user 消息现取，时间 = 文件 mtime。没有第二份真相。
    - 空会话不建文件（没说过话的不该在清单里占一行）；坏行跳过而不是整份作废（append-only 文件末尾可能是断电写了一半的）。
    - 恢复失败保持原会话不动：宁可命令没生效，也不能把人扔进一段空白历史。
    - 测试 core/tests/test_session.cpp（`ctest -R session`）；端到端实测：说一句话 → `/new` → `/resume <id>` → 追问"我刚才让你做什么"，模型据恢复的历史答对。

## M4：Provider 插件

> **实况注（2026-08-26）：本节整条作废**（[[ADR-0017]]）。「Provider 插件」这个形态在
> [[ADR-0016]] 就没了；ADR-0017 进一步把 LLM 那一块摊成「方向 × 协议」六个文件
> （`llm/upstream/` 与 `llm/downstream/` 各三个），协议由用户在配置里明选，
> 没有默认值也不从 `base_url` 猜。本节交付的能力都还在，形态全变了。

> **实况注（2026-08-25）**：协议构造、SSE 解析、计价三件事都还在，形态从"两个容器 + 一条 dep 边"
> 变成 `core/src/llm/llm.cpp` 里三个函数（[[ADR-0016]]）。**"改请求"那一段整段消失**——
> 它存在的唯一理由是协议层与供应商壳住在两个动态库里，中间必须留一道缝让后者补前者的空。
> 现在 `build_request` 直接读配置里的端点与凭证，一次造出能发的请求。
> 连带消失的还有"本次用的是哪个模型"那笔暗账：从前壳在 refine 时偷偷记下、计价时读出，
> 现在计价直接收模型名参数。

**交付物**
- v1-messages 插件（协议层）：`/v1/messages` 构造 + SSE 解析（成对，供应商中立，无默认端点/模型）
- deepseek 壳插件（供应商壳）：`deps: v1-messages`，包住协议层，兜底 DeepSeek 默认（嵌套链，ADR-0004）
- libcurl 出站流式请求（HTTP/1.1，出站 Provider 通信）
- 请求管线（core 收集 → 协议链入口构造 → 发出 → 解析 → 事件流）

**技术要点**
- 协议/供应商拆分：`/v1/messages` 是协议（多家公司共用），协议层不识别供应商；供应商默认值下沉到壳。
- 端点与模型名在 core 侧无默认（默认树里是空串），但**壳负责兜底**（ADR-0010）：core 不做必需键校验、不因缺配置退出。
- 可配置：api_key / base_url / model / small_model，全部可缺省，经 `.realagent/settings.json` 覆盖代码默认树，不读 env。base_url 与 api_key 同级重要（代理/网关用户必须能自定义端点）。
- DeepSeek 兼容端点细节（已查证）：tools/tool_use/tool_result 全支持；tool_choice 四模式全支持；stream 全支持；`cache_control` 被忽略；thinking 的 budget_tokens 忽略；thinking 块带 signature（回传历史用）。
- 壳不做模型映射：`claude-*` 原样透传（供应商特殊逻辑一律不进壳）。

**风险**
- **R10: DeepSeek /v1/messages 与 Anthropic 标准差异**
  - 决策：已解除（2026-08-09）——工具调用 + 流式完整支持（见上）。
  - 实现：调研结论，无独立实现物。
- **R11: 流式解析增量 → 事件流 message_update 的对接**
  - 决策：已定——增量与 thinking 同语义，都走推送流。
  - 实现：已完成（2026-08-10）——SSE 解析发 `thinking_start/update/stop`（core/src/llm/llm.cpp，2026-08-25 前在 realagent-plugins/v1-messages/v1_messages.cpp），接收器是 `Agent::on_llm_event`（core/src/agent/agent.cpp）。

## M5：QUIC 通信

**交付物**
- QUIC/HTTP3 server（core）+ quic-go client（TUI）。**实际选用 Cloudflare quiche**（core/CMakeLists.txt:17-19，`quiche_h3_*` API 贯穿 core/src/server/quic_server.cpp），不是本文与 ADR-0006 原写的 ngtcp2 + nghttp3；此换库无 ADR 记录。
- HTTP/3 REST 端点：以 docs/PROTOCOL.md 的端点表为准（那张表逐行标了实现状态）。首版实到 9 个路由（core/src/server/quic_server.cpp:271-330）；`POST /command`、`GET /sessions`、`POST /session` 至今未实现，会话管理走 `POST /message` 的斜杠命令。
  - 实况注（2026-10-06）：上一句已过时——这三个端点都在。server 的分发现在是一张路由表（`QuicServer::route`，注册全在 `core/src/main.cpp`），以 PROTOCOL.md 与 main.cpp 为准，本文不再记行号。
- 推送流（GET /events 长生命周期单向流，SSE 语义，全可靠）

**技术要点**
- 全可靠流：增量与结构化事件无差别，QUIC 可靠流原生保证，无自建确认机制（见 PROTOCOL.md 演进史）。
- 0-RTT 握手（重复连接握手与数据同发）。
- 数据报（RFC 9221）首版不使用，未来有真正可替换状态再引入。

**风险**
- **R12: HTTP/3 应用层**
  - 决策：已解决（2026-08-09）——要一个带 H3 语义的库，弃 msquic（纯传输层）。原记的选型是 ngtcp2 + nghttp3。
  - 实现：已实现，**但落地的库是 Cloudflare quiche**（core/CMakeLists.txt:17-19 `find_library(QUICHE_LIBRARY NAMES quiche)`，服务端全程 `quiche_conn_*` / `quiche_h3_*`）。决策与实现的库名对不上，且这次改选**无 ADR 记录**——待核实：是有意换库还是 ADR-0006 从未更新，需要写决策的人确认，我不替它编理由。
- **R13: 推送可靠性机制**
  - 决策：已定（2026-08-09，全可靠流）——全推送走 HTTP/3 可靠流（SSE 语义）。废弃数据报/时间戳水位/捎带发送（那是重复实现 TCP）。
  - 实现：已实现——`GET /events` 一条长生命周期 H3 流（core/src/server/quic_server.cpp:271-280，`content-type: text/event-stream`，`fin=false`）。全仓零 datagram 引用，废弃的那套机器确实没有被写出来。
- **R14: quic-go 支持度**
  - 决策：已解除——自 v0.27 支持 RFC 9221 数据报，HTTP/3 成熟（首版不使用数据报）。
  - 实现：选型结论，无独立实现物。客户端确实用 quic-go v0.61.0（tui/go.mod:10），未用数据报。

## M6：TUI

**交付物**
- Go + Bubble Tea 界面（参考 claude code / codex 客户端）
- 状态栏（输入框下方常驻栏，cometix 格式 `🤖 model | 📁 dir | 🌿 branch ✓`）：core 侧 `GET /statusline` 端点 + `statusline` 推送帧（见 docs/PROTOCOL.md 端点表与帧表），TUI 侧 model 在 tui/cmd/realagent-tui/main.go:92、渲染与本地 `/statusline` 配置命令在 tui/cmd/realagent-tui/statusline.go。
  **此项是对原计划的反转**：本文与 CONTEXT.md 原都写「无状态栏」（CONTEXT.md 已于 2026-08-16 订正）。**反转无 ADR 记录，且与现存 ADR 冲突**——docs/adr/0007-tui-go-bubbletea.md:34 至今写着「**无状态栏**（用户明确）」，没有任何 ADR 取代它，而代码已完整实现状态栏。理由未知，不在此代拟。
- quic-go 客户端
- 消息流渲染（流式打字效果，由推送流帧到达驱动）
- 输入框、审批对话框

**技术要点**
- TUI 职责纯粹：渲染 + 用户输入 + QUIC 客户端；一切 agent 逻辑在 core。
- Bubble Tea Model/Update/View 模式；异步事件（推送流）→ tea.Msg 桥接。

**风险**
- **R15: Bubble Tea 事件模型与 QUIC 异步事件流集成**（异步事件 → tea.Msg 的桥接）
  - 决策：已定——推送流帧转成 tea.Msg 喂 Update。
  - 实现：已完成（2026-08-10）——`subscribeCmd` 订阅 /events（tui/cmd/realagent-tui/main.go:139），帧分发在同文件 :480-530；审批模态 `permission_request` → y/n → `POST /approval-response`（main.go:522）。

## M7：集成与测试

**交付物**
- 全链路验证：工具注册 → LLM（DeepSeek）→ 权限 → 执行 → 回传
- 单元测试（C++ Catch2 + Go testing）
- 打包分发（core 二进制 + 插件 .so + tui 二进制）

---

## 风险清单汇总

**两列各说各的，互不蕴含**——一条风险「想清楚了」不等于「写出来了」，这张表此前把两者打成同一个 ✅，读者据此以为 R9 交付了，实际上会话存储一行代码都没有。

- **决策状态**：这个问题怎么解决，定了没有。取值：已定 / 已解除（问题不存在了）/ 已废弃（方案作废）/ 待定。
- **实现状态**：代码里有没有。取值：已实现 / 部分实现 / 未实现 / 无实现物（风险解除、废弃、纯调研结论这三类本来就没有交付物）。

本表所有实现状态均于 2026-08-16 逐条读码核实，出处见各里程碑「风险」小节。

| # | 问题 | 决策状态 | 实现状态 |
|---|---|---|---|
| R1 | Clang C++26 协程支持度 | ✅ 已解除（C++20 协程核心足够） | — 无实现物；附注：core 至今零协程 |
| R2 | msquic 集成 | ✅ 已废弃 | — 无实现物；替代品实为 quiche，非 ngtcp2（见 R12） |
| R3 | 插件事件订阅接口 | ✅ 已定（单入口分发） | ✅ 曾实现（`event.observe`）；2026-08-25 随 ADR-0016 删除，loader.cpp 已不存在 |
| R4 | 嵌套组装时机 | ✅ 已定 | ✅ 已实施（2026-08-10）：反图 BFS 逐层 init + `import`/`providers` 依赖注入（ADR-0012） |
| R5 | edit +x-0 的 LLM 描述 | ✅ 已定（创建语义进描述） | ✅ 已实现（现在 core/src/tools/edit.cpp；core_tools.c 随 ADR-0016 删除） |
| R6 | bash 流式输出 | ✅ 已定（tool_output 帧走推送流） | ✅ 已实现（2026-08-16）：容器 emit + TUI 续行渲染，端到端实测 |
| R7 | 事件流实现形态 | ✅ 已定（while(1) + fan-out） | ✅ 已实现（agent.cpp 的 `Agent::run`） |
| R8 | 中止传播模型 | ✅ 已定（LLM + 工具统一信号） | ✅ 已实现（2026-08-16）；2026-10-06 改为每 agent 一个中断标志，修掉多 agent 下的全局 pid 串扰 |
| R9 | JSONL 会话 schema | ✅ 已定；**落地时改为同形存储**（2026-08-16，理由见 M3 风险节） | ✅ 已实现（2026-08-16）：`Session` + `/new` `/resume` + `GET /sessions` `POST /session`，端到端实测 |
| R10 | DeepSeek 协议差异 | ✅ 已解除（工具调用 + 流式全支持） | — 无实现物（调研结论） |
| R11 | 流式解析 → message_update 对接 | ✅ 已定 | ✅ 已完成（2026-08-10）：thinking 三帧 + message_update + tool_use 打通 |
| R12 | HTTP/3 应用层 | ✅ 已解决（原记 ngtcp2+nghttp3） | ✅ 已实现，**但用的是 quiche**；换库无 ADR 记录，待核实 |
| R13 | 推送可靠性机制 | ✅ 已定（全可靠流） | ✅ 已实现（`GET /events` 一条 H3 可靠流，零 datagram） |
| R14 | quic-go 支持度 | ✅ 已解除 | — 无实现物（选型结论）；客户端用 quic-go v0.61.0 |
| R15 | Bubble Tea 事件桥接 | ✅ 已定 | ✅ 已完成（/events → tea.Msg + 审批模态） |
