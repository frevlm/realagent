# ADR-0026：传输改回 TCP —— 请求走 HTTP/1.1，推送走 WebSocket

- 状态：已采纳（2026-10-06）
- 取代：ADR-0006 的传输选择（QUIC/HTTP3、quiche、quic-go、0-RTT）；ADR-0002 里「quiche 非线程安全，agent 线程只入队、事件循环线程出队推送」；ADR-0010 第 7 条的触发时机（「每轮事件循环」改为「每条请求之后」）

## 背景

ADR-0006 选 QUIC 是为了公网：0-RTT、TLS、抗丢包。落地后一条没兑现：只绑 `127.0.0.1`；证书现生成、客户端 `InsecureSkipVerify`；没开 early data；回环上没有丢包。手写的 quiche 层反倒在流控满时只写一部分、剩下的字节静默丢掉，「全可靠」在应用层漏了。

回环上量过（同一个 handler 挂三种传输）：WebSocket 与 HTTP/1.1 的推送延迟是 h3 的一半到三分之二，大帧吞吐高 2 到 30 倍、每 GB 的 CPU 少约 3 到 60 倍，请求-响应与建连都快几倍（Apple M5 与 2 核 Linux 两台）。h3 只在「小帧不限速」上帧率更高，而 LLM 吐字到不了那个频率。

## 决策

1. **请求是普通 HTTP/1.1**，路由与请求体一律不变；**推送是 `GET /events` 的 WebSocket**，帧为 `{"event":"<type>","data":<json>}`。
2. **cpp-httplib**，钉 v0.57.1，与 realontext 的 Relay 同。TLS 不在 core 里：远程时放在前面的反向代理上。
3. **请求排成一队，一次处理一条**：一把锁替掉单线程事件循环，`when_idle` 当场回 `AGENT_BUSY` 的约定不变。推送任意线程直接写，事件队列与 `on_tick` 删除；状态栏载荷在每条请求之后比对（配置只在请求里改）。
4. httplib 默认开 `SO_REUSEPORT`，第二个 core 也绑得上——「端口就是那把锁」会被悄悄拆掉，改成只开 `SO_REUSEADDR`。
5. 推送连接的读超时设 0：httplib 默认 300 秒收不到客户端的帧就断，而客户端从不发帧。

## 后果

- 删掉 quiche、quic-go、证书生成。core 的第三方依赖是 libcurl + cpp-httplib。
- 写不完的字节 TCP 会阻塞而不是丢：一个卡住不读的客户端会拖住发帧的 agent 线程。本机先不管，上远程时给每条连接加发送队列。
- 推送连接断开，`has_client()` 立刻为假，审批当场拒绝——与从前订阅流断开时一样。
