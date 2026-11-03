# ADR-0033：子 agent 是一次阻塞的工具调用，删掉 agent 之间的有向图

- 状态：已采纳（2026-11-03）
- 取代：ADR-0019 §4（有向图）、§4b（`spawn` 立刻返回 + `send_message`）、§5 中「收件箱三种来源、hook 的订阅者由边推导」、§9（图不落盘——没有图了）
- 取代：ADR-0029 §代价里「完成通知不回声」那两条规则（没有完成通知了）
- 不动：ADR-0019 §1 / §2 / §6 / §7 / §8，ADR-0021（组），ADR-0029（客户端只认对话，帧带 `root` / `session_id`）

## 背景

ADR-0019 给模型的是一套 agent teams：`spawn` 立刻返回新 agent 的 id，模型自己填 `in_edges` / `out_edges` 决定谁能给谁发消息，`send_message` 往邻居收件箱里投信，跑完的 agent 沿入边回流完成通知。

实际用下来，这套东西的成本落在三处：

- **模型要当调度器。** 派一个子任务得先想清楚边怎么连；漏了自己的 id 就收不到完成通知，结果永远回不来。
- **完成通知是一条异步消息，不是返回值。** 父 agent 派完活往往直接收工，靠通知把它叫醒再跑一趟。有环的图会互相唤醒，ADR-0029 为此补了「只由通知唤醒的那一趟不再通知」「被中断的那一趟不通知」两条规则，又带出「中间层 agent 的回报会丢」这个代价。
- **core 里一套只为它存在的机制**：边表、能力校验（授不出自己没有的边）、发信人署名、`notice` 标记、`asked_`、`on_done` 广播。

Claude Code 的 subagent 是另一种形状：一个工具，给 prompt，**阻塞到子 agent 跑完**，它最后那段正文就是工具结果。子 agent 不能再派生，agent 之间没有消息通道。对「把一件独立的活交出去、拿回结论」这个真实用法，它够了，而且模型不需要学任何新概念——就是一次函数调用。

## 决策

### 1. `spawn` 阻塞，结果是子 agent 的最后那段正文

```
spawn {prompt, agent?, workdir?}  →  <子 agent 最后那段 assistant 正文>\n\n(session <id>)
```

- `prompt` 必填，是子 agent 唯一的输入；它看不见父对话。
- `agent` 照旧（ADR-0024 §8）：agent 定义的正文接在子 agent 的 system prompt 末尾。
- `workdir` 可选，默认是派生方自己的。多仓库协调仍是真实用法（ADR-0019 §3 的理由不变），但不再必填。
- 交回的是**最后那段正文，不是 recap**：正文是子 agent 照着 prompt 写给派生方的答案；recap 是小模型写给人看的摘要，留在 `agent_end` 帧里。
- 末尾的 `(session <id>)` 是给客户端的：回放时凭它把子 agent 的过程嵌回这张工具卡片下面（与改动前同一个形状，旧会话照样能回放）。

工具名不改。改名会让盘上的旧会话在客户端里认不出派生卡片，而名字对模型没有意义。

### 2. 子 agent 是那次调用的局部变量

`Agents` 只记打开着的对话。`Agents::spawn` 建出子 agent 交给 `Executor`，它不进表：投进 prompt、等它跑完一趟、析构（join 它的线程、归还 MCP 租约）。会话照旧落在对话那一头的 `sessions/sub/`，帧照旧盖父对话的 `root`。

于是「对话在不在跑」只需要问对话的 agent 本身：子 agent 跑着时，它正卡在 `spawn` 里。

### 3. 子 agent 不能再派生

子 agent 构造时不拿 `Agents`，`spawn` 不出现在它的工具清单里，它的 executor 也拒绝执行。照抄 Claude Code：一层就够，递归派生是成本失控的入口。

### 4. 中断往下传

`POST /interrupt` 停的是对话的 agent。它的 executor 被置中断标志后，正在等的 `spawn` 每 50 ms 看一次标志，看到就中断子 agent。**反复传而不是只传一次**：子 agent 开跑时会清掉自己的中断标志，中断恰好落在它开跑之前就会丢。

### 5. 删掉的东西

`send_message` 工具、边表与能力校验、发信人署名、完成通知、收件箱里的 `notice` 标记、`asked_`、`Agents::send` / `on_done` / `node`、system prompt 里的「Your agent id is N」。agent id 留着，只给审批认人用。

## 后果

- 模型侧：一个工具、一次调用、一个返回值。没有 id 要记，没有边要连。
- 失去的形态：常驻的 worker、agent 之间互发消息、teamwork 环。需要并行时父 agent 在一个 turn 里发多个 `spawn`，但工具严格顺序执行（ADR-0002），所以它们是串行跑的——要并行，是给 `spawn` 加后台模式的事，不是把图请回来。
- 子 agent 在 `ask` 权限下的审批照旧走它那一组的客户端，帧带它自己的 `session_id`，客户端看得出是子任务在问。
