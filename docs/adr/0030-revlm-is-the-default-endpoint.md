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
