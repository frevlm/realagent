# ADR-0029：客户端只认对话，agent 退回 core 内部

- 状态：已采纳（2026-10-08）
- 取代：ADR-0019 §3（`POST /agent` 对外一个端点）、§10（`GET /agents`、`GET /sessions` 的 `opened_by`）、§6 中「每帧带 `agent_id`、core 不为任何 agent 过滤」那一句
- 取代：ADR-0020 中「TUI 选一个 agent 连上去看」的读法——切的是对话，不是 agent
- 补充：ADR-0021（组照旧；组内寻址从 `agent_id` 换成 `session_id`）
- 不动：ADR-0019 §4 / §4b / §5 / §7 / §8（图、边、收件箱、idle 丢历史、审批）

## 背景

ADR-0019 把多 agent 做成了 core 里的一张有向图，并且把这张图原样交给了客户端：

- 客户端启动先 `POST /agent` 建一个 agent，拿到 `agent_id`，之后每个请求都带它
- gui 侧栏列出 `Agent 3 · realagent · 子 · 1,2`，TUI 有 `/agents` 面板
- 会话列表里一条会话「被 Agent 5 打开着」

这带来两个问题：

1. **用户看见的是调度器的内部结构。** 用户要的是「我跟它的那段对话」，不是「这台机器上第 3 号 agent，入边 1、2」。agent id、边、workdir 是给模型和 core 用的。
2. **空 agent。** 客户端一启动就建 agent，「新建 agent」按一次建一个。用户一句话没说，它们就占着线程挂到关组。要清理就得定义「空」是什么、什么时候扫、扫掉之后客户端手上那个 id 怎么办——全是补丁。

两个问题其实是同一件事：**协议里有两个名词（agent 与会话），而用户只有一个（对话）。**

## 决策

### 1. 两层地址

| 层 | 谁用 | 地址 | 管什么 |
| --- | --- | --- | --- |
| 图（core 内部） | 模型、`Agents` | `agent_id` | 边、收件箱、完成通知、`spawn` / `send_message` |
| 协议（客户端） | 人 | `session_id` | 对话内容、发消息、中断、审批 |

**图一个字不改。** 模型照样看得见 agent id（`spawn` 的返回值、`[from 3]` 署名、`send_message(to=7)`），能力模型照样成立——那是模型之间的事。ADR-0019 本来就写着「人不是图上的节点」，这里只是把这句话落实到协议上。

`session_id` 能当地址，是因为它与 agent 一一对应：一个会话同一时刻最多被一个 agent 打开（两个 agent 往同一个 JSONL 里追加，那段对话就毁了）。

### 2. agent 在第一条消息到达时才建

```
POST /message {client_id, workdir, session_id, message} → {status}
```

**`session_id` 由客户端生成。** 本组有 agent 打开着它，就投进那个 agent 的收件箱；没有，就建一个 agent 打开它——盘上有就接着说，没有就是一段新对话；别的组打开着，拒绝。

为什么是客户端给 id：要是由 core 在第一条消息到达时生成、放进回执里返回，agent 推的第一帧可能赶在回执前面（请求与推送是两条连接，没有先后可言），两个客户端就都得在「id 还没到」那段里攒帧。id 先在客户端手上，这段空档就不存在。id 的格式不承载任何意思——清单按 mtime 排，标题从内容里取。

**「从没发过消息的空 agent」因此在结构上不存在**——不需要清理定时器，不需要定义什么叫空。这个特殊情况不是被处理掉的，是被消除掉的。

`POST /agent`、`GET /agents`、`POST /session` 删除。`/new` 与 `/resume` 变成纯客户端命令：换一段对话不需要 core 做任何事，只是下一条消息带哪个 `session_id`。会话读写不再碰 agent 的锁，ADR-0017 那句「agent 正在运行——先中断」随之消失。

### 3. 每帧盖两个章：`session_id` 与 `root`

推送帧里的 `agent_id` 换成：

- `session_id`：这一帧是谁说的
- `root`：它属于用户看见的哪段对话

`root` 是每个节点创建时定下、之后不变的标签，与 ADR-0021 的组同一种推导：客户端开的对话，`root` 是自己的 `session_id`；`spawn` 出来的，继承创建者的 `root`。**它只管显示与中断，投递只看边**——不是第二份关于图的真相。

章盖在 agent 的事件出口上（`Agent` 构造时把 `emit_fn` 包一层），于是 `broadcast`、bash 的 `tool_output`、审批的 `permission_request` 一处都不用各自记得带。

客户端：`root` 是当前对话就画，`session_id ≠ root` 就画进子 agent 的块里；审批不管 `root` 是谁都弹，并说明是「子任务」还是「另一段对话」在问。

### 4. 子 agent 嵌在父对话里

子 agent 的会话落在**对话那一头**的 `sessions/sub/` 下，而不是它自己 workdir 的——记录归用户看见的那段对话。`GET /session {workdir, session_id}` 先找顶层再找 `sub/`。

回放时要知道哪个子 agent 是哪张 `spawn` 卡片派出去的：`spawn` 的工具结果写成 `7 (session <id>)`。模型照旧读得出 id，客户端从结果里认出会话，点开时再去读。没有另存索引——索引就是第二份真相。

### 5. 中断是整段对话的事

`POST /interrupt {session_id}` 停下 `root` 等于它的全部 agent。用户眼里那是一件事：Esc 按下去，子 agent 还在烧钱，对用户来说就是没停。

这不是 ADR-0019 否决的「级联 close」：没有引入创建关系的边，用的是 `root` 这个标签；也不 close，只 interrupt。

### 6. 客户端带齐三样

每个动对话的请求带 `client_id`、`workdir`、`session_id`。TUI 此前请求体里**从没带过 `client_id`**（只在 `/events` 的查询串里带），ADR-0021 的组隔离对它从来没生效过；这次在 `client.go` 的 `conv()` 里统一补上。

## 代价

- **打错的 `/resume <id>` 不报错**：盘上没有，下一条消息就开出一段用这个 id 的新对话。core 分不出「打错了」与「新的」，也不该去猜。
- **TUI 的子 agent 只显示动静，不铺正文。** 终端是一条行流，两段文字交错着长就乱了。TUI 里子 agent 是「↳ 子任务：…」「↳ 🔧 bash」「↳ 子任务收工」这样一行一条；全文在 gui 里展开看，或者等完成通知回到父对话。
- **用户换走的那段对话的 agent 留到关组。** idle 时历史已经还给盘，只剩一条线程；它派出去的子 agent 跑完还能把通知投回来。
- **`spawn` 的结果多了一段模型用不着的会话 id。** 换来的是回放时不需要索引文件。
