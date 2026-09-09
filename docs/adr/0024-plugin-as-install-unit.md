# ADR-0024：加入 Plugin —— 一个安装单元，唯一的容器

- 状态：已采纳（2026-09-07）
- 依赖：ADR-0016（插件体系废除）、ADR-0010（启动读一次，不热重载）、ADR-0018（不为纯文本另开工具）、ADR-0019（agent 必传 workdir）、ADR-0021（组随客户端生灭）、ADR-0022（skill 是提示词目录）、ADR-0023（MCP 只声明接口）
- 不取代任何 ADR。修正 ADR-0023 一处（`${...}` 一律不展开 → 白名单一个）

## 背景

ADR-0016 在 2026-08-25 铲平了插件体系。ADR-0022 加 skill、ADR-0023 加 MCP，
开头都得先回答那句「这不就是插件换了个名字吗」。这一次连名字都不换了，所以那句得答得更硬。

逐条对 ADR-0016 那张税目表：

| ADR-0016 列的税目 | 旧插件 | 本 ADR 的 plugin |
| --- | --- | --- |
| C ABI | 手写 70 行 JSON 提取 + 30 行转义 | 无。一个目录、几份 markdown、两份 JSON |
| 借阅 / 转移 | 每个跨界 `const char*` 都要管所有权 | 无跨界指针 |
| 异常不得穿越 ABI | 边界上转状态码，否则 `terminate` | 无边界 |
| 能力槽解析 | `resolve_slots` 130 行回答「管线四段谁来干」 | **管线一段不换**。plugin 只提供数据，不选择谁干活 |
| 命名空间前缀 | 工具对外一个名、对内一个名，`executor.cpp` 里一段注释解释这不矛盾 | 名字只有一个。前缀是拼出来的那一个名字的一部分 |
| 撞名检查 | `O(n²)` 防不存在的第三方 | 前缀天然隔离，同名整条覆盖，无比对 |
| 外部仓库 | 加载器 + SDK + `plugin.json` 规范 + 两套 CMake | 无加载器、无 SDK。布局是 Claude Code 的事实标准 |

**判据一字未改：core 里有没有多出一行不是自己写的代码。**

旧插件是 core `dlopen` 别人的 `.so` 再调它的函数。本 ADR 的 plugin 交出来的全是数据：
一段提示词、一段命令正文、一张启动规格表。**唯一一处 core 主动执行第三方代码是 hook**，
那一条单独定价，见 §6。

而 ADR-0016 那条决定性的理由——「插件体系服务过的用户数是 0，五个容器全是本项目自己写的」——
在这里同样不成立。判据是**现存的 Claude Code plugin 能不能 clone 下来直接吃**。
这台机器上就装着五个，一个都不是本项目写的。**能不能吃下它们，是这份 ADR 唯一的成败标准**——
本 ADR 的每一条取舍都由它推出来。

## 决策

### 1. plugin 是安装单元，也是唯一的容器

`~/.realagent/` 与 `<workdir>/.realagent/` 本身就是 plugin，只是没有名片的那两个（下称**隐式 plugin**）。
于是「两处来源」这个说法作废，代码里只剩一种东西：一个有序的 plugin 列表。

```text
~/.realagent/plugins/*/            装来的，按目录名排序
<workdir>/.realagent/plugins/*/    仓库带的
~/.realagent/            → "user"     隐式
<workdir>/.realagent/    → "project"  隐式，最近
```

扫盘在 agent 创建时发生一次，与 workdir 同期确定、同期不变（同 ADR-0022 §3）。
**不在 `build_dialog` 里扫**——那会让一趟中间 system prompt 变形，prompt cache 当场碎掉。

**扫完就没有 plugin 了。** 手里剩下的还是那三张表（skill 清单、工具表、命令表），
每一项多一个字段说明它从哪来。`Executor` 不认识 plugin，`build_dialog` 不认识 plugin，
`handle_command` 不认识 plugin。**plugin 一旦变成运行期对象、参与调用路径，能力槽就回来了。**

### 2. 目录即安装

装 = 把目录放进去（`git clone`），卸 = 删掉，更新 = `git pull`。

**不做 marketplace、不做安装状态文件、不做版本锁。** Claude Code 那套是个包管理器
（`known_marketplaces.json` + `installed_plugins.json` + 版本化的 `cache/` 目录），
而 ADR-0016 铲掉的税目里就有一份「哪个装了、哪个关了」的状态（`plugins.disabled`）。
core 是常驻服务，没有拉 git、报进度、处理私有仓库认证的位置。

两级目录天生就是 Claude Code 的 user / project 两级 scope，不用为它设计任何东西。

真要 `/plugin install`，一个外部小脚本就够——它做的事就是 `git clone` 到那个目录，core 一行不改。

**名字取目录名**，不取 `plugin.json` 里的 `name`：文件系统已经保证唯一，读出来再比对
只是给自己造一类要处理的错误（同 ADR-0022 对 Agent Skills 规范那个字段的处理）。

**`plugin.json` 只是名片，不描述自己有什么**——有什么由目录结构说了算。
Claude Code 允许把 hooks 内联进 `plugin.json`，本项目不跟：同一份数据两个落点，
迟早只改一边。代价是内联写法的 plugin（这台机器上的 caveman 就是）hooks 吃不下。

### 3. 前缀是 plugin 的一个字段，不是它的本质

| plugin | 前缀 |
| --- | --- |
| `user` / `project`（隐式） | **空** |
| 装来的 | 它的目录名 |

**一个字段，两种取值，没有分支。** 既有的名字因此一个字符都不变，而第三方之间靠隔离消歧。

这是本 ADR 与 ADR-0022 §2 / ADR-0023 §4 唯一分歧的地方，分工写死：

- **同一个 plugin 内部**照旧「同名近的覆盖远的」——同一个人写的两份，他知道自己在覆盖谁。
- **两处都装了同名 plugin**，仍旧近的覆盖远的——同名就是同一个 plugin 的两个版本。
- **不同名的 plugin 之间不覆盖**——两个陌生人各写各的，静默覆盖谁都查不出为什么。

前缀顺带逼出一处早就该做的分家：**名字不属于连接**。从前 `McpClient` 拿着整条规格
（含 `name`），于是「同一条命令、两个键」是两个进程。加了前缀之后这不再是个边角情况——
两个 plugin 各带一份一模一样的 server 是常态，尤其在 §5 的 http 桥接下（同一个 url
归一出同一条命令）。规格因此收成三个键（`command` / `args` / `env`），
**它自己就是连接的键**（ADR-0023 §2 那句话到这儿才字面成立），名字挂在租约上。
**共用进程不等于共用名字**：工具名照旧靠各自的前缀分开。

推论：**「装来的会不会盖掉我自己写的」这个问题不存在。** 隐式 plugin 前缀为空、
装来的带前缀，两边的名字撞不上，所以扫描链上装来的排前还是排后，一个字都不影响结果。
顺序只在前缀相同的两站之间起作用。

MCP 工具名因此是 `<plugin 前缀>_<配置的键>__<server 那头的原名>`。
分隔符只有一个答案：**单下划线连的是「身份」那一半，双下划线之后才是原名**——
这是 Claude Code 自己的形状（`mcp__plugin_cloudflare_cloudflare-bindings__d1_database_create`），
不发明第二种。名字撞上端点 64 字符的上限就让它撞：报一条人话，**不截断**，
因为截断会让名字随算法变，而它要永久写进会话记录。

### 4. `${CLAUDE_PLUGIN_ROOT}` 展开，其余照旧不展开

ADR-0023 写着 `${...}` 一律抛错，理由是「连接是进程级的，不属于任何目录」。
**那条理由只打得中一类变量。**

| | 它问什么 | 有没有唯一答案 |
| --- | --- | --- |
| `${workspaceFolder}` / `${CLAUDE_PROJECT_DIR}` | 当前项目目录 | 否。N 个 agent，N 个答案，展开它就是让访问边界漂移 |
| `${CLAUDE_PLUGIN_ROOT}` | 这个 plugin 装在哪 | 是。一个 plugin 一个安装位置，跟谁在调用无关 |

判据因此不是「是不是变量」，是**这个变量有没有唯一答案**。白名单一个名字，不是一套变量系统。

不展开它的代价不是不方便，是**分发单元不成立**：那份 `.mcp.json` 是 plugin 作者写的，
他不知道你的用户名。ADR-0023 那句「写死绝对路径不是委屈他，是让他明确地授一次权」
对用户自己写的配置成立，对 clone 来的不成立。

**授权动作因此换了位置：从「写一行配置」变成「装这个 plugin」。**
它在隐式 plugin 里出现就是错——那儿没有 plugin root，见到就抛。

在 command 正文里同样展开：现成的 command 常引用自己目录里的脚本，同一条判据、同一个白名单。

### 5. `type: "http"` 在归一那一层变成一条 stdio 规格

这台机器上唯一一份真实的 plugin `.mcp.json`（cloudflare 的），五个 server 全是 `http`。
按 ADR-0023 现在的代码，那个 plugin 装进来 MCP 那半边一个都起不来。

```text
{"type":"http","url":"https://x/mcp"}  →  {"command":"npx","args":["-y","mcp-remote","https://x/mcp"]}
```

**特殊情况在 `parse_entry` 里消失，下游一行不动**：没有第二种 transport，
没有 curl POST、没有 SSE 解析、**没有 OAuth**，读线程与按 id 认领那一套一字不改。
自己实现 http 的真实成本不是那次 POST（core 已经链着 libcurl），是 OAuth 2.1 那一块——
浏览器回调、token 存储、刷新，而 core 是常驻服务。

白捡一条性质：同一个 url 被两个 plugin 引用会共用一个桥接进程，因为归一后那份 JSON 一样，
`dump()` 出来的键就一样。

桥接命令是**配置里的模板，有真默认值**（同 ADR-0016「默认值可以是真的了」）：

```jsonc
"mcp_http_bridge": ["npx", "-y", "mcp-remote", "{url}", "--header", "{name}: {value}"]
```

**带 `{name}` / `{value}` 的参数按 header 数量重复，一个 header 都没有时整段不出现**——
一条规则把 header 并进同一个模板，不为它另开一个键。

**core 不硬编码任何包名**：硬编码会变成「core 决定去跑一个它没写的程序」，
而 MCP 的判据是**命令由用户在配置里点名**。换个桥接工具改一行；改成空 = 不支持 http，
遇到报一条人话。

占位符**不写成 `${...}`**：那个形状在本项目专指别家客户端的变量，两者字面上就该分开。

### 6. hook 是唯一一处 core 主动执行第三方代码

先把它和 MCP 分清楚：**MCP 进入的是「工具」这个已经存在的洞**——模型主动挑一个调，
`Executor` 多一个分支就完了；**hook 要在 core 的生命周期点上自动跑**。这是真区别，
必须单独定价。

但它**不是能力槽**：槽是「管线这一段由谁来干」，独占、必须有人填、填错管线就断；
hook 是旁挂，可以零个，没有它管线照跑。`resolve_slots` 那 130 行不会因此长回来。

**形状是一个位置问题**：

```text
run(Pre*, payload)  →  原来那件事  →  run(Post*, payload)
```

**一个 `run(event, payload)` 返回一个 Outcome（`deny` / `reason` / `inject`）**，
位置由事件名决定，签名不分家——写成 `run_pre` / `run_post` 两个函数的话，
后者的返回值永远被忽略，那是个骗人的接口。没装 hook 时 `run` 立即返回空 Outcome，
零开销（同 ADR-0022 §6：一个 skill 都没有时 system prompt 一字不差）。

**只在真有落点的地方成对**：

| 事件 | 落点 |
| --- | --- |
| `PreToolUse` / `PostToolUse` | `Executor::execute` 前后 |
| `SessionStart` | agent 创建 / `/new` / `/resume` 之后 |
| `UserPromptSubmit` | 从收件箱取出一条 user message 时 |
| `Stop` | 模型调了 `stop`，`agent_end` 之前。**只观察，不改控制流**——出口仍然只有一个 |

`PreCompact` / `PostCompact` 不适用（本项目没有压缩）。**不为对称造名字**：
`SessionEnd` 在多 agent 里指哪件事答不上来，就别造。

**`deny` 只能收紧，不能放宽。** hook 说 deny 就 deny，说别的一律当没说过。
于是没有优先级表，它退化成权限链上的一个「与」，**一个 hook 永远不能把 `ask` 变成 `allow`**——
「装了个 plugin，危险工具突然不问了」在设计上不可能发生。post 位置收到 `deny` 要报一条，
不静默丢（ADR-0023 §2 骂过的那件事，不能自己再犯）。

**一次事件一次进程**，照抄 Claude Code。现成的 hook 脚本读 stdin 到 EOF 就退，
常驻能省掉进程启动，但那要定义一条自己的协议，现成脚本一个都跑不了。

**中断时直接杀。** hook 进程是这一个 agent 独占的，跟进程级共享的 MCP server 不是一回事，
杀它不会弄断别人。

**原来那件事没做完，post 就不跑**（抛了、被中断）；工具报错算做完了，post 要跑并拿得到那个结果。
判据是**有没有产出一个结果**——不用 RAII，它在这两种情况下的行为恰好是反的。

`PostToolUse` 的 payload 带工具结果，可能是几 MB。**截到定值并标 `"truncated": true`**，不静默截。

**装即授权**：hook 不经 permission、不走审批。用户把这个 plugin 放进目录，
就是同意了它在这些点上跑——再问一遍是把一次明确的授权拆成每次都要重复的骚扰。
**这一条是本 ADR 最重的一笔，不藏在别处。**

### 7. command 是一条消息的模板，不是一条命令

`command.hpp` 原来的契约写着「命令不投收件箱，直接返回结果」，而 Claude Code 的 command
就是一段 prompt，它必须触发一趟 run。

| kind | 是什么 | 走哪条路 |
| --- | --- | --- |
| `builtin` | core 的一个动作 | `try_lock` → 执行 → 直接返回结果 |
| `prompt` | 一段文字 | 展开 → `post()` 投收件箱，**不拿锁** |

**那个 `try_lock` 要挪进 builtin 那一支**：prompt 命令等价于用户打了一段字，
而 `POST /message` 本来就不拿锁。让它回 `AGENT_BUSY` 是把发消息这件事变得比原来更难。

派发多**一个**分支，判的是「core 的一个动作，还是一段要发出去的文字」。

**内置的不可被覆盖**（理由同内置六个工具：覆盖掉 `/new` 就没法开新会话）。
装来的带前缀天然不撞，只有隐式 plugin 撞得上，那一条跳过并报出来。

从 md 里只取三样：`description`、`argument-hint`、正文。`allowed-tools` / `model` **不收**——
权限与模型档位不该由一段 markdown 决定（同 ADR-0023「core 从 server 手里只取三样，其余自己写」）。
参数只认 `$ARGUMENTS`：它进的是 prompt 不是 shell，与用户自己打那段字没有区别。

### 8. `agents/<name>.md` 只是一段接在后面的文字

`spawn` 加一个可选参数 `agent`：给了名字，就把那份正文接在被派生 agent 的 system prompt 后面。

不加新工具（ADR-0018 那条不变），不加执行路径。`tools` / `model` 两个字段不收，理由同 §7。

`name` 也不收：**名字取文件名**，与 skill 取目录名、command 取文件名同一条规则。
一份文件在盘上叫什么，模型看见的就是什么——多一个可以与文件名不一致的字段，
就多一种「清单里那个名字在盘上找不到」的坏法。

**core 那段（agent id / workdir / `stop` 契约）永远在前**。这不需要额外保证：
`Agent` 只有一个类、一个 `system_prompt()`，派生出来的走同一条路，
拿不到「不带 stop 契约」的 system prompt。

### 9. 内置的不参与

内置六个工具、内置三条命令不是 plugin，不进扫描链，不可覆盖。

CONTEXT.md 的原话是「内置的六个随 core 一起编译，**少一个就是 core 坏了**」。
把它们放进一张可被覆盖的表，就是给「core 坏了」开一条配置路径——
一个 plugin 覆盖掉 `stop`，agent loop 当场没有出口。

## 代价

**装一个 plugin 是一次完整授权。** clone 下来之后，它的 hook 会在下次建 agent 时跑，
它的 MCP server 会被 fork/exec，它的 command 会进命令表。中间没有第二次确认。
这是 §6 那条「装即授权」的正面表述，也是本 ADR 换来「装一个目录就能用」的价钱。

**解不了的一律只报不修**，不加机制：`npx` 首次下包在 agent 创建时同步阻塞；
桥接的 OAuth 要用户当场开浏览器而提示只进 stderr；工具名撞 64 字符上限而三段用户都改不动。
这些进 `/plugins` 的错误列表。

**skill 从 `~/.realagent/skills/` 挪进一个 plugin，名字会从 `foo` 变成 `plugin:foo`。**
这是 §3 隔离语义的必然后果，搬家的人自己承担。

**测试结果会随跑测试的机器上装了什么 plugin 而变**：测试把 `HOME` 指到临时目录。

## 迁移

现有用户的 `~/.realagent/skills/`、`~/.realagent/mcp.json`、`<workdir>/.realagent/*`
**一个字都不用动**——它们成为隐式 plugin，前缀为空，名字与行为一字不差。

新增的都是可选目录，一个都没有时行为与本 ADR 之前完全相同。

## 实现过程中改掉的三个判断

写代码把三处纸上说得通、落地不成立的地方顶了回来，记在这儿而不是悄悄改掉：

**一、「装来的排前，你自己写的覆盖你装来的」是空话。** 加了前缀之后隐式与装来的
名字撞不上，扫描链上装来的排前还是排后一个字都不影响结果。顺序只在前缀相同的
两站之间起作用。

**二、`{name}` / `{value}` 那条 header 规则认不出组边界。** 先按「带占位符的参数按
header 重复」写，结果 `--header` 自己不带占位符，零个 header 时它孤零零留在命令行上。
边界从参数自身看不出来，只能写出来——见 §5 的子数组。

**三、`type: "http"` 那条路自己拼了 `name`，绕过了前缀。** 根因是「加前缀」写在了两处。
收成一处（读配置那一层算一次，往下传），http 与 stdio 从此拿到的是同一个名字。
这条是端到端测试抓出来的——单元测试各自都是绿的。

## 落地

七步全部完成，C++ 11 个测试、Go 全绿。

1. 纯重构：扫描链（`plugin.hpp` / `plugin.cpp`），skill 与 MCP 并进去。行为零变化，测试一个未改
2. `plugins/*` 两站 + 前缀 + `${CLAUDE_PLUGIN_ROOT}`
3. `commands/*.md` + 派发那一个分支 + `GET /commands` 的可选 `agent_id`
4. `type: "http"` 桥接
5. `hooks/hooks.json` 五个落点
6. `agents/*.md` + `spawn` 的 `agent` 参数
7. `/plugins`（只读）

最后一步是一份端到端测试：造一个**逐字照抄 Claude Code 布局**的 plugin 目录，
五样成员一次吃下。那份测试就是本 ADR 的成败标准本身。

`plugin.json` 直到第 7 步才读——它是名片，在那之前没有任何消费者，
提前读它就是造一个没人看的字段。
