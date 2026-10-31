# ADR-0032：收工由 System One 判，小模型只写 recap

- 状态：已采纳（2026-10-30）
- 依赖：ADR-0010（不做档位间回落）、ADR-0025（收工判定）、ADR-0030（revlm 是默认端点）
- 部分取代：ADR-0025 §2「判定与 recap 是同一次调用」

## 背景

ADR-0025 让小模型一次调用交两样：recap 与 done，并在 JSON 里把 recap 排在 done 前面。
这把「写一段话」和「回一个是非」绑在一起：小模型要写 200 字的 prose 才能给出一个布尔，
回来的东西还要宽容地解析（前后闲话、`"false"` 字符串、不是 JSON），`parse_stop_verdict` 与 `truthy` 就是为这些存在的。

revlm 现在有 System One：`POST /v1/systemone`，一问一答，直接回问题的答案，是非题回一个把握（`noul`）。

## 决策

### 1. 第三档模型 `decision_model`

`ModelTier::Decision`，键 `decision_model`，与 `model` / `small_model` 共用 base_url 与 api_key，**不回落**（ADR-0010）。

### 2. 判 done 的是 decision_model，只走 revlm

主模型一轮没调工具时，先问 System One：

- 请求不是聊天协议，不进 `Protocol` / `SseParser`，由 `llm/systemone` 造请求、读答案，`send_revlm` 发；
- 输入 `state = {user, assistant}`：这一趟 user 的话（不含 `[supervisor] ` 打回的）与最后一条助手正文，各截 8000 字符，别的一概不给；
- 问 `done`（`noul` 型），`answers.done.noul >= 0.5` 为收工；
- 钱照 `Pricing` 算进这一趟的花费。

### 3. 没干完就打回，且不问小模型

打回理由是固定的一句，走原有的 `[supervisor] ` 路径。打回这一圈没有 recap 可写，小模型一次不问。
`kMaxStall` 不变：连着三轮没跑工具仍强制收工。强制收工也是收工，小模型照写 recap——邻居要知道这一趟停在哪。

### 4. 干完了才让小模型写 recap

小模型（`small_model`）拿这一趟的抄本写 recap，回的就是 recap 本身，不是 JSON。
`parse_stop_verdict`、`truthy` 与判定那一半 prompt 一并删除。`StopVerdict` 的形状不变，`agent_end` 与完成通知不用改。

### 5. 判不出来仍然收工

`decision_model` 没配、`protocol` 不是 `revlm`（**直接结束，一次请求都不发**）、调用失败、答案缺了或形状不对，一律判收工：
判不出来不该把人扣在循环里花钱。`small_model` 没配则没有 recap，同 ADR-0025。

## 后果

- 非 revlm 端点不再有收工判定：没调工具就是干完了。这是有意的——System One 是 revlm 的能力，不为第三方端点另造一套裁判。
- 打回不再带「还差什么」，主模型只被告知没干完。换来的是判定一次是非题的调用，不是一次写作。
- 引导页多一步 `decision_model`；`small_model` 的文案改成「写 recap」。

System One 那次调用限 30 秒（`kDecisionTimeoutS`），超时按收工：它没人看着，卡住这一趟就永远收不了工。recap 与主模型调用照旧不设限。
