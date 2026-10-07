# core 内置能力清单

> 从前这份文档叫 `plugins.md`，列的是 5 个动态库容器。ADR-0016 废除插件系统之后，
> 它们是 core 里的几个 `.cpp`——**能力还是那些能力，只是不再隔着 C ABI**。
> ADR-0017 又把 LLM 那一块按「方向 × 协议」摊成了六个文件。

## 1. LLM 调用（`core/src/llm/`）

三套协议，两个方向，六个文件（ADR-0017）：

```
llm/upstream/{anthropic_messages,openai_chat,openai_responses}.cpp   造请求
llm/downstream/{anthropic_messages,openai_chat,openai_responses}.cpp 解响应
llm/llm.cpp                          派发 + SSE 切块 + 端点配置校验 + 计价
```

```
对话 ──build_request──▶ HttpRequest ─[agent 用 libcurl 发出]─▶ 响应流
                                                                 │
                    Pricing::cost(model, usage) ◀── usage ◀──feed_block
```

### 协议是用户选的，不猜也没有默认

`protocol` 只有三个值：`anthropic-messages` / `openai-chat` / `openai-responses`。
它与 `base_url`、`model` 合称**端点束**，三个键都没有默认值——不填就完全用不了，
而填错产生的报错最难自己诊断（端点 404、请求体形状不认、流解析出空）。
缺键时 core 照常启动，但启动日志与任何一次 `POST /message` 都会把该填什么原样告诉你。

一套协议管着四样**共变**的东西，所以它们由一个值选定，不拆成四个开关：

| | `anthropic-messages` | `openai-chat` | `openai-responses` |
| --- | --- | --- | --- |
| URL | `{base_url}/v1/messages` | `{base_url}/chat/completions` | `{base_url}/responses` |
| 认证头 | `x-api-key` **与** `Authorization: Bearer` 同时发 | `Authorization: Bearer` | `Authorization: Bearer` |
| system | 顶层 `system` | `messages[0]` 里 `role=system` | 顶层 `instructions` |
| 一条消息 | 块数组 | 字符串 + 另开 `tool_calls` | 强类型 item |
| 工具参数 | 对象 | JSON **字符串** | JSON **字符串** |
| 工具结果 | `tool_result` 块 | 另开一条 `role=tool` | `function_call_output` item |
| 帧类型在 | `data` 的 `type` 字段 | `data` 的 `choices[].delta` | SSE 的 `event:` 行 |
| 流的终点 | `message_delta` | `data: [DONE]` | `response.completed` |
| token 字段 | `input_tokens` / `output_tokens` | `prompt_tokens` / `completion_tokens` | `input_tokens` / `output_tokens` |

`anthropic-messages` 两个认证头一起发是实测的结果：无效 key 打 Anthropic 原厂，
一个头与两个头的错误响应逐字相同（原厂对多出来的 `Authorization` 视而不见），
而 DeepSeek 一类兼容端点走的正是 Bearer。同一凭证的两个名字，
于是不必为"对面是哪一家"开配置项，也不必在代码里认 URL。

### 事件词汇表只有一套

产出 `thinking_start` / `thinking_update` / `thinking_stop` / `message_update` /
`tool_use` / `usage` / `stop`。协议是三套，agent 与 TUI **不跟着分三份**——
`finish_reason: "tool_calls"` 在下行文件里就归一成 `tool_use`，
`prompt_tokens` 换算成 `input`，出了那个文件没人知道它原来叫什么。

- **一次调用一个解析器实例**：上一轮的半截 SSE 缓冲绝不该漏进下一轮。
- **畸形帧一律报错，绝不静默跳过**：跳过 = 正文/tool_use 悄悄消失，用户拿到一个
  "成功但空"的回答且没有任何提示。`feed` 返回 `false`，`llm_call` 中止本次调用。
  流内的错误帧（`openai-chat` 的 `{"error":...}`、`openai-responses` 的
  `response.failed`）同样按失败处理——HTTP 是 200，但这次调用没成。
- **异常不得穿出**：解析跑在 libcurl 的写回调里，中间隔着 C 栈帧。
  异常在函数内兜住转成返回值——这条约束不随插件系统消失，它是 libcurl 的。
- **先看状态码，再谈解析**：`curl` 对 401/500 一律返回 `CURLE_OK`。
  非 2xx 的响应体不是流，攒起来给人看，不喂解析器（ADR-0017）。
- **一个字都没有的回答不入会话**：空 text 块写进 JSONL，下一轮原样回传，
  端点拒收，那个会话从此每轮都报错。

### `Pricing`：模型数据表与计价（ADR-0009）

token 用量 × 该模型单价 = 钱。**usage 事件本身不上传**——客户端不认识 token，只认识钱。
算不出（表里没这个模型）就什么都不发，不发 0。本次用的是哪个模型，
由调用方直接传进来（就是 `dialog["model"]`）。计价与协议无关——钱就是钱。

两个来源，**不合并**：`~/.realagent/models.json` 存在就是它，否则用编译进二进制的出厂表。
半份表比没有表更难查，所以用户想改一个模型的单价就得连表一起接管。
解析严格：条目缺字段即报错，不跳过坏条目、不补默认值。报错时不留半份表，
core 照常启动——没有表只是不算钱，不是不能对话。

**公开清单**（`/model` 与状态栏的数据源）只有 `name` / `owned_by` / `context`，**单价不出去**。

## 2. 工具（`core/src/tools/tools.cpp` + `core/src/mcp/`）

**两个来源，一个词**（ADR-0023）：内置的六个随 core 一起编译，
[MCP server](../docs/adr/0023-mcp-tools-only.md) 交出来的那些随外部进程生灭。
模型眼里没有这条界线——一份清单、同样的 Schema、同样的 `tool_use`。

工具定义**就是端点要的那个对象**（`name` / `description` / `input_schema`），
外加一个 `_core` 键装 core 私有的字段（`label` / `dangerous`，MCP 来的还有
`server` / `remote_name`）；发出去之前 `erase("_core")`。**不是结构体**——
MCP 递来的本来就是 JSON，转成结构体再转回去是两次互相抵消的转换。

结果只有一种形状：`{"content": [块...], "isError"}`，也是 MCP 那个。
带不动块数组的协议在 `llm/upstream/<协议>.cpp` 里压平（ADR-0023 §3）。

### 内置那六个

LLM 见到的就是短名——没有命名空间前缀这回事了。

| 工具 | 职责 | 安全属性 |
|---|---|---|
| `read` | 读文件 → 每行带行号与行 hash（`142 29c 正文`），整个文件、不截断 | 只读 |
| `search` | 调 `realontext query` 搜 workdir 的代码 → 整段函数与类，按 `pattern` 匹配、`query` 重排；realontext 二进制随 core 内置（ADR-0026） | 只读 |
| `edit` | 把第 `line` 行换成 `new_text`（`hash` 对得上才动手）；`edits` 数组可跨多文件 | 危险（过权限检查点） |
| `bash` | 执行 shell 命令，回传 stdout **与 stderr**（合流） | 危险（过权限检查点） |
| `spawn` | 派生一个 agent 去干一件事，**立刻返回它的 id，不等它跑完**；`in_edges` / `out_edges` 由模型决定（ADR-0019 §4b） | 危险（过权限检查点） |
| `send_message` | 把一条消息投进另一个 agent 的收件箱；只能发给自己有出边的那些 | 只读（不碰文件、不起进程） |

一趟的出口**不在这张表里**：主模型没有下一步动作时，由小模型看着这一趟的抄本判收工与否
（ADR-0025）。~~`stop` 工具~~已删除——让干活的那个模型兼任裁判，要在 system prompt 里
写契约、配范例、忘了再补一条提醒，三处补丁治的是同一个病根。

### MCP 来的那些（ADR-0023）

配置两处来源、同名近的覆盖远的（`~/.realagent/mcp.json` 与 `<workdir>/.realagent/mcp.json`），
信封逐字抄事实标准。**连接进程级**，一份配置一个，按配置身份去重。
只说 `2026-07-28`：没有 `initialize` 握手，起进程之后第一句话就是 `tools/list`。

名字是 `<配置的键>__<server 那头的原名>`，非法字符换 `_`，**不截断**（转发用的仍是原名）。
**一律带危险标记**——规范自己说 `annotations` 不可信。
坏 server 跳过、报错原文进 stderr，core 照常起。

**无独立 `write` 工具**——创建是 `edit` 的一种用法（不给 `line` 即写整个文件），
与替换、换成多行、删除共用同一个操作（ADR-0018）。工具描述里写清了四种用法。

**行 hash 的性质**（ADR-0018）：FNV-1a 取 3 个十六进制字符，**只算这一行**、空白不参与。
不带邻域、不防撞、不做全表、不做回捞——判断只是一句 `if`：按行号取那一行、算它的 hash、
一致就改。因此逐条应用天然正确（改第 1 行不影响第 2 行的 hash），格式化跑一遍不作废，
而文件别处增删行导致的行号漂移会让 hash 对不上、模型重读一次。

**`spawn` / `send_message` 实现在 `Executor`，不在 `tools.cpp`**：它们要认识
`Agents`，而 `tools/` 在 `agent/` 下面，反过来包含就是层级倒挂。**定义仍在同一张表里**
——LLM 看见的工具清单只有一份。

**执行细节**：
- 单 agent 内工具严格顺序执行（ADR-0002）。
- **stdout 与 stderr 合流**（ADR-0017）：`dup2` 两条都接同一个管道。只接 stdout 的话，
  命令失败时模型收到的是"退出码非零，无话可说"——报错原文全在 stderr，
  而那是它判断该不该重试、怎么改的唯一依据。合流而不是两个管道：交错顺序就是人在
  终端里看见的顺序，而"哪一行来自哪条流"没有第二个读者。
- bash 输出实时走 `tool_output` 帧（推送流，全可靠）：读循环每读到一行就发一帧
  `{call_id, stream: "output", text}`，命令完整输出仍在 `tool_result` 里回传——
  两者不是二选一，帧是给人看的实时反馈。超上限后只吞不推（管道还得读干净，
  中途撒手等于给命令一个 SIGPIPE）。
- **可中断**：bash 子进程自成**进程组**（`setpgid`），中止时打的是整组——首次 SIGTERM
  给命令一个自己收尾的机会，用户再按一次就 SIGKILL，命令拉起的子孙树不留孤儿。
  顺序执行意味着同时至多一个子进程，一个 pid 就记得住，不需要表。
- bash 输出上限 50KB，超出末尾标 `...`。**`read` 不限大小、不截断、不分页**（ADR-0018）：
  本地读一遍内存，限制的是不存在的问题；bash 那条是管道，性质不同。

## 3. 权限（`core/src/agent/executor.cpp`）

一个配置键 `permission`，一个 `switch`：

| 值 | 行为 |
|---|---|
| `ask`（默认） | 危险工具一律问用户 → `permission_request` 帧 → `POST /approval-response` 回传（ADR-0005） |
| `allow-all` | 一律放行。打通链路用，**不是安全策略** |
| `deny` | 一律拒绝 |

认不出的值按 `ask` 处理并点名：配置写错时该多问一句，不该多放一次行。
只读工具（`dangerous = false`）根本不进这个检查点。

ASK 走审批协调器（`approval.hpp`）：agent 线程真等裁决（30s 超时按 deny），
事件循环线程收到裁决后唤醒它。**core 永远是审批发起方**，这条 ADR-0005 的结论没变。

## 4. 斜杠命令

全部是 core 内置：`/new`、`/resume`、`/model`。

`/plugins` 与 `/provider` 随插件系统一起删除——前者没有对象了，后者是配置里的
`base_url` 一行字。

> `/quit` 与 `/statusline` 不归 core：退出的是客户端进程，core 是常驻服务、还连着别的客户端。
> 它们是 TUI 本地命令，照常出现在斜杠菜单里，但从不发给 core。

## 5. Skill（`core/src/agent/skills.cpp`）

一个目录 + 一份 `SKILL.md`，**纯提示词**（ADR-0022）。core 不加载、不执行、不给它任何执行语义——
只把名字、描述、绝对路径写进 system prompt，正文要不要读由模型决定，用现成的 `read` 读。
**没有第七个工具**。

```
~/.realagent/skills/<name>/SKILL.md            全局，跟着人走
<workdir>/.realagent/skills/<name>/SKILL.md    项目，跟着仓库走（同名时它赢）
        │
        └─ agent 创建时扫一次 ─▶ Skill{name, description, path} ─▶ system prompt 每个一行
```

形状照抄公开的 [Agent Skills 规范](https://agentskills.io/specification)：YAML frontmatter +
Markdown 正文，`name` 必须与父目录名一致——所以名字直接取目录名，解析器唯一的活是拿 `description`。
frontmatter 用 vendored 的 fkYAML（`core/include/fkYAML.hpp`，单头文件，与 `json.hpp` 同一条路子，
`find_package` 仍旧是三个）。

坏 skill（没有 frontmatter、解析失败、没有 description）不进清单，错误原文进 stderr，
**agent 照常创建**——挑的是 `models.json` 那条先例，不是 `settings.json` 那条。

规范里的 `scripts/` core 不管：跑脚本的是模型手上的 `bash`，照 `dangerous` 那条走权限检查。

## 验证闭环

```
core → build_request（对话 → /v1/messages 请求，端点凭证取自配置）
     → libcurl → 端点
     → SseParser（SSE → thinking / message_update / tool_use / usage）
     → Pricing::cost（用量 → cost）→ status_update
     → 工具（read/edit/bash）→ 权限检查点 → permission 配置裁决 → 执行
     → tool_result 回传 → 下一 Turn（thinking 块带 signature 原样回传历史）
```

单元测试：`core/tests/test_llm.cpp`（造请求 / SSE 解析 / 计价，不碰网络）、
`core/tests/test_skills.cpp`（扫盘 / frontmatter / 同名覆盖 / 坏 skill 跳过）、
`realagent-core test-tools`（工具执行 + 权限全链路）。
