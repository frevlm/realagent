# ADR-0036：压缩（compact）把历史换成一条带标记的摘要

- 状态：已采纳（2026-11-04）
- 取代：PLAN 里「上下文压缩：第一版不做」；ADR-0024 §6 与 CONTEXT.md 中「`PreCompact` / `PostCompact` 不适用」
- 不动：ADR-0025 / ADR-0032 的 [[Recap（回顾）]]——它仍然只读历史、不回写历史；会话 JSONL「一行就是内存里的一条」

## 背景

上下文一满，这一趟就死在端点的 400 上：没有任何路能继续。状态栏已经在报 `context_tokens` 与窗口占用（ADR-0009 的模型表有 `context`），缺的只是到点时做点什么。

参考过的四家（Claude Code、Codex、Pi、opencode）结论高度一致：

- 写摘要的是**主模型**；Codex 把总结 prompt **追加在现有对话末尾**，前缀不变，缓存照样命中。
- 用户说过的话受特殊保护（Codex 原样保留最近 20k token 的 user 消息，Pi / opencode 的模板里有专门一段）。
- thinking 一律不进；工具结果截断。
- 三种触发：手动 `/compact [指令]`、到阈值自动、端点报溢出时压一次再重试。
- 差别在「压完留什么」：Claude Code 只留摘要；Pi / opencode 另外原样留最近一截（要在落盘格式里加「从哪条开始保留」的指针）。

本项目的特殊约束有两条：会话文件与内存同形（`session.hpp`），以及主对话走 revlm 的字节级增量上下文协议（`ContextSession`）。

## 决策

### 1. 压完只剩一条摘要，摘要是一条带 `[compact] ` 标记的 user 消息

```
压缩前：messages_ = [m0, m1, …, mN]
压缩后：messages_ = [{"role":"user","content":[{"type":"text","text":"[compact] <摘要>"}]}]
```

- 落盘：这条消息照常 `append` 进 JSONL。之前的行一个字不动、不删、不改写。
- 读回：`Session::read` **从最后一条 `[compact] ` 消息读到文件尾**。没有标记的旧会话读法不变。
- 回放：`Session::read_frames` 仍给整份文件（压缩前的过程照样能翻），`to_frames` 遇到标记消息吐一个 `compact` 帧，客户端画一条分隔线。
- 标记的惯用法与 `kSupervisorTag` 相同：凡是从外面来的输入都是 user，靠正文前缀认出来源，不往消息里加键（加键会原样发给端点）。

**不保留最近一截原文。** 保留就要么在文件里把尾巴再抄一遍（盘上出现两份），要么加 `firstKept` 指针（破坏同形）。Claude Code 只留摘要，够用。

### 2. 写摘要的是主模型，读的是抄本

三种触发走同一条路：把历史转成抄本（复用 `verdict.cpp` 的裁法——user 原话与上一条摘要钉住不丢，其余按行截断，超预算从旧到新丢，thinking 不进），连同总结指令（带上 `/compact` 的指令与 `PreCompact` 注入的文字）作为一条 user 消息，单独发给主模型。

不把总结指令追加在原对话末尾（Codex 的做法）。那样前缀不变、缓存全命中，但原对话已经塞不下时（溢出）它发不出去，就得再备一条抄本路——两条路换来的只是写摘要那一次请求便宜些。抄本有上限（`kCompactTranscriptCap`，约 25k–30k token），任何时候都发得出去，一次请求的花费也有上限。

摘要正文的段落（取 Pi / opencode 的交集）：

1. 用户原话（这一段对话里用户的每一条请求，逐字）
2. 目标与约束
3. 进度：已完成 / 进行中 / 卡住
4. 关键决定
5. 涉及的文件（路径原样）
6. 下一步

第 1 段是收工判定的依据，见 §5。

### 3. 三种触发

| 触发 | 条件 | 压完之后 |
|---|---|---|
| `manual` | `/compact [指令]` | 有消息就接着答，没有就收工 |
| `auto` | 一圈开头、调主模型之前：上一次主模型调用的 `input + cache_read + cache_write + output` ≥ `context × compact_threshold / 100` | 接着这一圈 |
| `overflow` | 主模型调用被端点以「上下文超长」拒绝 | 重跑这一圈。压过只剩摘要还超，`compact` 说没得压（历史不到两条），照常报错收工——不需要另记「压过一次」 |

`compact_threshold` 是 `settings.json` 的整数键，**以 5 为一档，取值 50、55、…、100，默认 85**。不在档上的值启动即报错（读不懂就别猜，ADR-0010）。`100` 不是特殊值，只是阈值等于窗口——效果上就是「不提前压，只在溢出时压」，不需要另设开关。

模型表查不到这个模型的 `context`：没有窗口就没有阈值，自动压缩不发生；手动与溢出照常。

估算只用上一次 usage，不估算本圈新加的工具结果：大块结果把请求推过窗口时由 `overflow` 兜住，不为它造一个按字节估 token 的函数。

### 4. hook：`PreCompact` 与 `PostCompact`

形状照抄 Claude Code，`matcher` 比对 `trigger`：

| 事件 | 时机 | stdin | 能做什么 |
|---|---|---|---|
| `PreCompact` | 发摘要请求之前 | `{agent_id, cwd, session_id, trigger, custom_instructions}` | deny（退出码 2 / `{"decision":"block"}`）= 这次不压；inject = 追加进总结指令 |
| `PostCompact` | 摘要落盘之后 | `{agent_id, cwd, session_id, trigger, compact_summary}` | 只观察 |

`overflow` 触发被 `PreCompact` deny 时，这一圈照常以溢出错误收工——hook 选择了不压，就不该假装还能跑。

压完不再跑 `SessionStart`：Claude Code 那样做是因为压缩会冲掉注入过的上下文，而这里 `SessionStart` 的注入在 system prompt 里，压缩不碰它。

ADR-0024「只在真有落点的地方成对」的原则不变：压缩前后都真有落点，所以成对。

### 5. 一趟中途压缩后的收工判定与 recap

`run_begin_` 是「这一趟开跑时历史的长度」，压完历史只剩一条，它就指错了。压缩后置 `run_begin_ = 0`：System One 的 `user` 字段与 recap 的抄本从摘要那条开始取。摘要第 1 段逐字保留了用户原话，裁判依然知道「用户要的是什么」。

### 6. 手动压缩在 agent 线程上做

`messages_` 只在 agent 自己的循环线程上动。`/compact` 是 builtin 命令，它只往 agent 里放一个「要压缩」的请求（带指令）并唤醒循环；循环在一圈开头看到请求就压。idle 时这会开一趟：`agent_start` → `compact_start` → `compact` → `agent_end`，不调主模型干活、不走收工判定。

历史少于两条（没什么可压）不发请求，用 `turn_end {error}` 说一句。

### 7. 帧

- `compact_start {trigger}`：开始压，客户端显示进行中。
- `compact {text}`：压完了，`text` 是摘要。实时与回放同形（ADR-0020）。
- 失败不另造帧：沿用 `turn_end {error}`。

## 后果

- 撞窗口不再是死路：先在阈值处自己压，漏网的由溢出兜底再压一次。
- 盘上的历史只增不改：压缩是追加一行，回放能翻到压缩之前。
- 每次压缩都会打碎一次 prompt cache 与增量协议的 base；写摘要那次请求本身几乎不花输入钱。
- 摘要是有损的：最近一截原文、工具结果的细节都没了。这是「只留摘要」的代价，换来的是落盘格式不变。
- 没做的：不调模型就清掉旧工具结果（Claude Code 的 microcompact）、后台预先算好摘要（opencode）、保留最近原文（Pi / opencode）。
- 待实测：三种 downstream 是否接受「摘要 user + 新来的 user」两条相邻的 user 消息；各端点「上下文超长」的报错长什么样（Anthropic `prompt is too long`、OpenAI `context_length_exceeded`、revlm 待查）。
