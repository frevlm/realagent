# ADR-0030：默认端点是 revlm；`revlm` 是第四个协议名

- 状态：已采纳（2026-10-29）
- 部分取代：ADR-0017 §2（「端点那一束没有默认值」）

## 背景

ADR-0017 不给 `protocol` / `base_url` 默认值：对面是谁 core 不知道，给默认就是替用户猜。
revlm 是我们自己的网关（生产实例 `https://www.revlm.com`），两头都是我们的，默认指向它不是猜。

此前连 revlm 要配 anthropic-messages 端点再开 `revlm_context` 开关——两个键能配出
「开关开着、对面是第三方」的无效组合。

## 决策

1. **`protocol` 多一个值 `revlm`**：请求体与帧就是 anthropic-messages，区别只在传输
   （revlm::net + 增量上下文协议）。`revlm_context` 开关删除。
2. **默认 `protocol = "revlm"`、`base_url = "https://www.revlm.com"`**；`model` 仍无默认，
   可用模型取决于 token 绑的渠道组。
3. **引导页选 `revlm` 不问地址**，core 在 `/setup`、`/setup/models` 里把 base_url 定成生产实例。
   revlm 没有 OAuth，api_key 手填控制台建的 token。
4. **模型清单一律从端点拉**（`endpoint_models`：anthropic 线格式拉 `<base_url>/v1/models`，
   openai 两套拉 `<base_url>/models`），引导页与 `/model` 共用，revlm 与自定义端点同一条路。
   模型数据表只补 `owned_by` / `context`。拉不到就报原因、退回手填；`/model <名字>` 不查清单。
5. 第三方端点照旧选另外三个协议名，走 libcurl，不带 revlm 头。

## 后果

- 已有 `settings.json` 都写了 `protocol` 与 `base_url`（ADR-0017 的迁移要求），行为不变。
- 端点没有模型清单接口时，`/model` 不再列出厂表，改为报原因让人手填。
- 手写过 `"revlm_context": true` 的配置改成 `"protocol": "revlm"`。

## 修订（2026-10-30）：revlm 下协议按模型取

§1「请求体与帧就是 anthropic-messages」作废。revlm 的渠道各说一套（`messages` /
`chat_completions` / `responses` / `systemone`），网关按路由挑渠道、不做协议转换，所以同一个
token 下不同模型必须打不同路由。

- revlm 的 `/v1/models`（含 `/v1/models/:id`）每条加 `protocol`，取模型所在渠道的协议名。
  一个渠道组里一个模型只挂一种协议，这是运维约定，revlm 不校验。
- core 的 `call_protocol(cfg, model)`：`protocol=revlm` 时按模型查这张清单，进程内记住，
  没见过的模型再拉一次；换了 base_url 或 api_key 整张作废。缺 `protocol` 字段直接报错，
  不回落到 messages——两头都是我们的，先发 revlm。
- 主模型与小模型各查各的，可以说不同的协议。
- 拉清单与 LLM 调用走同一条传输（revlm::net，说不上 h2 退回 curl），`HttpRequest` 因此多了
  `method` 与 `timeout_s`，`post_*` 更名 `send_*`。

### 待定：`systemone`

revlm 的第四种渠道协议 `systemone`（TypeSafe System One，模型 `jev-*`）今天走
`call_protocol` 的「realagent 不会说这套协议」那条报错。原因不是缺适配，是能力对不上：

- 它不是对话模型。请求是 `{model, state, questions}`，`questions` 里每个问题是
  `noul`（是/否概率）/ `choice`（单选）/ `score`（有序打分）之一；响应是
  `{model, answers, usage}`，只有概率与选项，没有正文，`output_tokens` 恒为 0（免费）。
  文档没提流式、`messages`、system prompt、工具。见 Databricks《Query models with the
  TypeSafe System One API》。
- 一次 Turn 要求模型产出正文或 `tool_use`，所以它当不了 `model`。收工判定要 `{recap, done}`：
  `done` 形状上就是一个 `noul`，`recap` 是正文，它给不出来，所以也当不了完整的 `small_model`。

两条路，未决：

1. 认出 `systemone`，报一句准确的错：「这是 System One 分类模型，只回结构化答案，不能当
   model / small_model 用」。一行映射加一条报错。
2. 把收工判定拆开：`done` 交给 Jev 的 `noul`（快、便宜），`recap` 仍由文字模型写。一次判定
   变两次调用，等于改写 ADR-0025。
