# ADR-0035: 思考量级是一个配置键，值原样透传

- 状态：已接受
- 日期：2026-11-04
- 影响：ADR-0010（新增配置键 `effort`，默认树里没有它）、ADR-0017（三个 build_request 各多一个字段）

## 背景

三套协议都早已能解析思考流（`thinking_*` 事件），但上行从不说「想多深」。各家对这个旋钮的取值已经收敛到同一组词：`none | minimal | low | medium | high | xhigh | max`，每个模型认其中一个子集；DeepSeek、Gemini 兼容层、OpenRouter 还会在服务端把没有的档位就近归并。分歧只剩**字段放在哪**：

| 协议 | 字段 |
|---|---|
| anthropic-messages | `output_config.effort`，配 `thinking: {type: "adaptive"}` |
| openai-responses | `reasoning.effort` |
| openai-chat | 顶层 `reasoning_effort` |

另有两处坑：Claude 新模型的思考正文默认不下发（`display: "omitted"`），流里只有块尾签名；Responses 不要 `reasoning.summary` 就没有可读的推理事件。两者都会让 💭 什么也画不出来。

## 决策

1. **一个配置键 `effort`，字符串，只作用于主档**。`Agent::build_dialog` 在 `ModelTier::Main` 时把它放进 dialog；recap、System One 自己造 dialog，天然不带。子 agent 走主档，随之继承。
2. **不做映射表，不做取值校验**。值原样交给协议层，协议层只决定字段位置。端点不认的值由端点报错。
3. **没配就不发**。默认树里没有这个键，请求体与引入前逐字节相同。`/effort default` 写回空串，回到这个状态。
4. 发 effort 时顺带要回可读的推理：anthropic-messages 带 `display: "summarized"`，openai-responses 带 `summary: "auto"`。
5. 只有签名、正文为空的思考块照样落盘（`record_assistant`），签名不能在回传时丢掉。
6. `/effort [level]` 改配置；statusline 载荷带上 `effort`，经已有的「载荷变了才推」通道到客户端。

## 后果

- 不认识 effort 的模型（如 Kimi K2.x 拒收 `reasoning_effort`）会让请求失败，经 revlm 时看到的是它换遍渠道后的 502，而不是上游原文。用户用 `/effort default` 退回。
- Qwen 的整数 `thinking_budget`、Kimi K2.x 的 `thinking: {type}` 不在这三种形状里，这次不支持。
- 思考 token 计入 `max_tokens`，原先写死的 4096 不够。anthropic-messages 这个字段必填，改为 64000；openai-chat 里它可选，删掉，由端点用模型自己的上限。
- openai-chat 上行把 thinking 块回传为 assistant 消息的 `reasoning_content`：DeepSeek 文档要求带 tools 时每轮都回传，缺了 400。在 openai-chat 上，DeepSeek 不带 `reasoning_effort` 就不思考，配了 effort 才走到这条路。
