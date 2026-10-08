# ADR-0028：gui 是 Wails 桌面应用 —— 网页直连 core，core 按 Origin 放行

- 状态：已采纳（2026-10-08）
- 依赖：ADR-0006（core 是服务，客户端只是客户端）、ADR-0021（一个客户端 = 一组）、ADR-0026（HTTP/1.1 + WebSocket）

## 背景

gui 跟 TUI 一样只是 core 的又一个客户端：连 core，不内嵌 core。界面用 Web 技术写（TS 生态），桌面端要一个壳。

壳的候选（同一页面、macOS、RSS）：Electron 444MB / 包 307MB；系统 WebView 约 195MB / 外壳几 MB。
系统 WebView 的壳里，Wails 的原生侧是 Go——TUI 已经是 Go，不再为几十行原生代码多养一套工具链。

网页直连 `127.0.0.1:12345` 有两处要 core 配合：

1. 页面的源（`wails://wails`）不是 core 的源，读响应要 CORS 头；
2. core 的 GET 从 JSON 体读参数，而 `fetch` 不许 GET 带体。

另有一处与 gui 无关、但直连逼着要面对的事：core 没有认证，也不看 `Origin`。用户浏览器里的任何网页都能往
`127.0.0.1:12345` 发 `text/plain` 的 POST（不触发预检），也能开 `/events` 的 WebSocket（不受 CORS 管）——
读到 `permission_request` 的 id 再替用户批了它。

## 决策

**`gui/` 是一个 Wails v2 应用**：

- **网页层**（`gui/src/`）：TypeScript strict + Vite，无框架。直接 `fetch` 与 `new WebSocket` 连 core（`core.ts`），
  推送断了 1.5 秒后重连。协议帧写成以 `event` 为判别键的联合类型，推送帧与 `GET /session` 回放走同一个 `handle()`（ADR-0020）。
- **Go 侧**（`gui/main.go`）只给网页三样东西：`Env()` 返回 core 地址、`client_id`、workdir；应用退出时发 `POST /group/close`。
  - `client_id` 一个进程一个：网页重载不换组。
  - core 地址默认 `127.0.0.1:12345`，环境变量 `REALAGENT_CORE` 覆盖。
  - workdir 默认是进程的 cwd（与 TUI 同）；从 Finder / 开始菜单启动时 cwd 是根目录，退回家目录；环境变量 `REALAGENT_WORKDIR` 覆盖（`make dev-*` 传仓库根）。
- **core 按 `Origin` 放行**（`core/src/server/server.cpp`，HTTP 与 WebSocket 升级共用一道）：
  - 不带 `Origin`（TUI、curl）放行；
  - Wails 页面的源放行：`wails://wails`（macOS / Linux）、`http://wails.localhost`（Windows）、`wails://wails.localhost:34115`
    （`wails dev` 下的窗口）、`http://localhost:34115`（`wails dev` 给浏览器开的页面），
    并回 `Access-Control-Allow-Origin`，`OPTIONS` 预检回 204；
  - 其余一律 403。
- **GET 的参数可以放查询串 `?body=<JSON>`**：还是同一份 JSON，只是浏览器发不出带体的 GET，换个地方装。请求体非空时以请求体为准。

浏览器里看 gui 只走 `wails dev -browser`（`make dev-browser`）：Go 侧的绑定在那个页面上照样可用，页面只有一个固定的源。
白名单里只有本机端口，不收任何网站——放进去的每一个源都能替用户跑 bash。

视觉用 frevlm 设计系统：token 原名照抄成 CSS 变量，Inter / JetBrains Mono 字体随包分发，离线可用。

## 渲染引擎

Wails v2 用各平台的系统 WebView：macOS / Linux 是 WebKit（WKWebView / WebKitGTK），Windows 是 WebView2（Chromium）。
三端渲染**不保证一致**。网页层只用标准 DOM API 与 `fetch` / `WebSocket`，换壳不动 `gui/src/`——只有 `core.ts` 里
`window.go` / `window.runtime` 两个入口和 core 的 Origin 白名单认识 Wails。

## 后果

- 协议加了一处：GET 可用 `?body=`；TUI 不变。core 多了 Origin 检查，除了上面那几个源，浏览器页面从此连不上 core。
- `wails dev -browser` 的页面上，Wails 自己的 `ipc.js` 会报 `Cannot read properties of null (reading 'nodes')`：那是它的断线遮罩组件，
  不影响绑定，属于 Wails v2.16 上游的问题。
- 新增工具链：Node（构建网页）与 Wails CLI（`go install github.com/wailsapp/wails/v2/cmd/wails@v2.16.0`）。
  `make gui` 出 `.app` / 安装包；`make dev-app` / `make dev-browser` 起 core 加热重载的 gui（窗口 / 浏览器）。
- 外链交给系统浏览器（`runtime.BrowserOpenURL`），不在应用窗口里跳走。
