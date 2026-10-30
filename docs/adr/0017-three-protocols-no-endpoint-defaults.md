# ADR-0017：协议按方向三分家；端点那一束不给默认值

- 状态：已采纳（2026-08-26）；§2 被 ADR-0030 部分取代（protocol / base_url 默认指向 revlm）
- 部分取代：ADR-0010（「没有"必需键"这回事」那半句）、ADR-0016（「装完即可用」那半句）
- 依赖：ADR-0016（插件废除后，协议知识第一次完整地住在 core 里）

## 背景

ADR-0016 落地后审了一遍代码，五个缺陷都不是拆插件拆坏的，四个在拆之前就在：

| | 症状 | 上机验证 |
| --- | --- | --- |
| bash 只接 stdout | 命令失败时模型收到「退出码非零，无话可说」，报错原文进了 core 自己的终端 | `echo BOOM >&2; exit 1` → `status=1 messages=[out\n]` |
| HTTP 状态码没人看 | 401 的 `CURLcode` 是 0，错误体不是 SSE，切不出事件块 → 一次认证失败被当成「内容为空的成功」 | 打回 401 的端点：`CURLcode=0 HTTP=401 事件数=0`，`llm_call` 返回 true |
| 空回答入会话 | 上一条的后果：`{"type":"text","text":""}` 写进 JSONL，下一轮原样回传，而端点拒收空 text 块——那个会话从此每轮都 400 | 会话文件里真的躺着一条空 assistant 消息 |
| 事件循环卡在 `agent_mtx` | run 期间发任何斜杠命令，事件循环那唯一一条线程去等锁，期间收不了 `POST /interrupt`——唯一能解开它的东西进不来 | `/command` t=1s 发出 t=15s 返回；`/interrupt` t=3s 发出 t=15s 返回 |
| `read` 分不清读满与截断 | 只读上限个字节，`gcount` 两种情况都等于上限 | 50000 与 50001 字节的文件读回来一模一样 |

同时暴露一处**文档超出实现**：ADR-0016 与 `CONTEXT.md` 写着「Anthropic、DeepSeek、OpenRouter
都实现同一端点，换一家就是改配置里的一行」，而认证头写死 `Authorization: Bearer`——
Anthropic 原厂的 API key 走 `x-api-key`，Bearer 那条路只认 OAuth token。
它点名的第一家就不成立。

## 决策

### 1. 协议按「方向 × 协议」六个文件，重载派发

```
core/src/llm/upstream/{anthropic_messages,openai_chat,openai_responses}.cpp
core/src/llm/downstream/{anthropic_messages,openai_chat,openai_responses}.cpp
core/src/llm/llm.cpp        派发 + SSE 切块 + 计价（协议无关）
```

一个协议在一个方向上的全部知识住在一个文件里：认证头、URL 路径、请求体形状、
帧结构、token 字段名。**这些是共变的**——拆成独立配置项就能配出无效组合
（Bearer 头配 OpenAI 请求体配 `/v1/messages` 路径）。绑成一束，无效组合不可表达。

派发用标签类型重载，不用虚函数：每个协议的解析状态自带 `using tag`，
`SseParser` 里一个 `variant` + 一次 `visit`，`llm.cpp` 里一句
`if (是不是 anthropic)` 都没有。

**事件词汇表只有一套**（`thinking_*` / `message_update` / `tool_use` / `usage` / `stop`）。
协议是三套，agent 与 TUI 不跟着分三份——`finish_reason: "tool_calls"` 在下行文件里
就归一成 `tool_use`，出了那个文件没人知道它原来叫什么。usage 同理：
`prompt_tokens` / `input_tokens` 在各自文件里换算完，出门一律叫 `input`。

### 2. 端点那一束（`protocol` / `base_url` / `model`）没有默认值

**默认值在这三个键上是垃圾设计**：不填完全用不了，而填错产生的报错
（端点 404、请求体形状不认、流解析出空）恰是最难自己诊断的一类。
给个默认等于替用户猜，猜错了他还以为是自己配的——报错了却啥都不知道。

判定标准是「不填就完全用不了」。按同一把尺子量，其余键**保留**默认：

- `api_key`：本地端点（llama.cpp 一类）不填就是能用；填错了服务端回 401，是句清楚的话
- `small_model`：缺了只是杂活不跑，主链路照常
- `permission`：这是**安全**默认，缺了不该放行——与「填了才能用」是两码事

**不在启动时退出。** core 是常驻服务、TUI 是另一个进程：core 一退，用户在 TUI 里
看见的是一句「连不上」，比配错了还难诊断，恰好犯了本条要治的病。
所以照常起、启动日志喊一遍，同时把那段话原样交给任何一次 `POST /message`——
让它出现在用户正盯着的那块屏幕上，缺哪个报哪个、一次报全、附一段能直接抄的样例。

协议也不从 `base_url` 猜。猜是「协议层认出对面是谁」，正是 ADR-0016 刚赶走的概念。

### 3. 认证头归协议自己，`anthropic-messages` 两个头一起发

`x-api-key` 与 `Authorization: Bearer` 同时发，同一个凭证的两个名字。
实测原厂对多出来的那个头视而不见（无效 key 打过去，一个头与两个头的错误响应
逐字相同），而 DeepSeek 一类兼容端点走的正是 Bearer（它们的文档让你设
`ANTHROPIC_AUTH_TOKEN`）。于是不必为「对面是哪一家」开配置项，也不必在代码里认 URL。

`openai-chat` / `openai-responses` 只发 Bearer——那是它们协议自己的规矩。

### 4. 先看状态码，再谈解析

`curl` 对 401/500 一律返回 `CURLE_OK`：HTTP 层的失败不是传输层的失败。
写回调首次拿到响应体时问一次 `CURLINFO_RESPONSE_CODE`，非 2xx 就把响应体攒起来
给人看，**不喂解析器**——错误体不是流，喂进去只会攒出一个「成功但空」的回答。

状态码 0 不算错：那是压根没拿到响应（连不上、被中断掐断在响应头之前），
由 `CURLcode` 去解释。在那儿说「HTTP 0」是拿一个不存在的状态码糊弄人。

**一个字都没有的回答不入会话**：空 text 块写进 JSONL 就是一块砖，
下一轮原样回传，端点拒收，会话从此报废。报错、不落盘。

### 5. bash 的 stdout 与 stderr 合流

`dup2` 两条都接到同一个管道。报错原文是模型判断该不该重试、怎么改的唯一依据。
合流而不是两个管道：交错顺序就是人在终端里看见的顺序，两个管道各读各的会把因果打乱，
而「哪一行来自哪条流」没有第二个读者。`tool_output` 帧的 `stream` 字段随之
从 `"stdout"` 改为 `"output"`——一条流，就别留一个说谎的名字。

### 6. 事件循环线程碰 `agent_mtx` 一律 `try_lock`

拿不到锁就回一句「agent 正在运行——先中断再执行这条命令」。
**一次拒绝是一句话，一次假死是没有话。** `POST /interrupt` 本来就不碰这把锁，
于是那条逃生通道任何时候都畅通。

### 7. `read` 多读一个字节

只读上限个字节的话，「文件恰好这么大」与「文件更大被截断」`gcount` 相同，
分不出来就只能一律当截断，于是恰好读满的文件被无辜削三个字节。
多读一个，这个特殊情况就没了——不是加分支，是让分支没有存在的理由。

## 代价与保留

**三套协议里今天真跑的只有一套。** `openai-chat` 与 `openai-responses` 的用户数是 0——
这正是 ADR-0016 砍掉插件用的理由。区别在于这次的税低得多：三对 `.cpp`、同进程、
无 C ABI、无借阅转移、无撞名检查、无外部仓库，加起来不到 700 行，
且每一行都有测试盯着。**这句话记在这里，是为了让后人知道我们没忘记自己刚说过什么**——
如果哪天这两套协议仍然没有用户、却开始收利息（比如为它们改公共接口），
那就该按 ADR-0016 的同一把尺子把它们删掉。

**「没有必需键」不再成立。** ADR-0010 取消必需键校验的**理由**是
「必需键校验把『供应商默认值下沉到壳』架空了」——壳在 ADR-0016 里没了，
那个理由跟着壳一起过期。理由过期的规矩该重写，不该供着。
现在的规矩是：`Config::load()` 仍然只在 JSON 读不懂时失败（配置**读取**层不认识业务），
「端点配齐了没有」是 `llm` 模块的知识，由 `endpoint_config_error()` 单独回答。

## 迁移

现有 `~/.realagent/settings.json` **必须加一行 `protocol`**，否则 core 起得来但不干活：

| 你的 `base_url` 长这样 | 加这一行 |
| --- | --- |
| `.../anthropic`（DeepSeek 兼容端点、Anthropic 原厂） | `"protocol": "anthropic-messages"` |
| OpenAI 风格的 `/v1`（`/chat/completions`） | `"protocol": "openai-chat"` |
| OpenAI Responses（`/responses`） | `"protocol": "openai-responses"` |

`base_url` 与 `model` 本来就配过的不用动；从没配过的，现在必须配。
不加也不会静默跑错——core 会把该加什么、抄哪一段，直接告诉你。
