# ADR-0027：搜代码是内置工具 —— 调 realontext CLI，不走 MCP

- 状态：已采纳（2026-10-06）
- 依赖：ADR-0018（工具越少选错越少）、ADR-0019（workdir 每 agent 一个）、ADR-0023（MCP 只接工具）

## 背景

realontext 交出两种接法：`realontext mcp`（一个 stdio MCP server，工具名 `codebase-retrieval`）
和 `realontext query`（一条命令，打印结果，退出码 0 命中 / 1 没命中 / 其余是错）。

走 MCP 的话它是一个 MCP 工具，于是：

1. **一律带危险标记**（ADR-0023）——每搜一次都过权限检查点，而它是只读的；
2. 名字变成 `realontext__codebase-retrieval`，还要模型每次传一个绝对路径 `directory`，
   而 agent 的 workdir core 本来就知道；
3. 多一个常驻进程、多一份 `mcp.json` 配置，坏了是「环境的事」——可搜代码跟读文件是一类事。

## 决策

**`search` 是第六个内置工具，跟 `read` 并排**（`core/src/tools/search.cpp`）：

- 参数只有 `pattern`（必填，rg 语法）与 `query`（可选，英文描述，给了就由 Relay 重排）；
  **没有目录参数**，搜的永远是 agent 的 workdir。
- 实现是 `run_proc` 跑一次 `realontext query --path . --pattern … [--query …]`，cwd = workdir。
  `--path .` 必须给：不给的话 CLI 看见 stdin 是管道就去搜 stdin。参数单引号转义后交给 `/bin/sh`。
- 只读，不带危险标记。中断照 `bash` 那样杀进程组；60 秒超时，输出上限同 `bash`。
- 退出码 1（没命中）**不算失败**，回一句「放宽 pattern」；127 报没装；其余把 stderr 原文回给模型。

### realontext 随 core 一起发

npm 包 `realontext` 只是个 Node 外壳，干活的是平台包 `@realontext/<平台>` 里的原生二进制。
**core 内置的就是这一个二进制**：`core/CMakeLists.txt` 在 configure 时从 npm registry 拉
钉死版本的平台包（darwin-arm64 / linux-x64）、校验 sha512，放到 `realagent-core` 旁边。

core 启动第一件事是把自己所在目录排进 `PATH` 最前（`main.cpp`，早于任何线程）。于是 `search`
调的、模型在 `bash` 里敲的 `realontext` 都是内置那一个，不看用户有没有全局装。
升级 = 改 `REALONTEXT_VERSION` 与两个 sha512，不跟 npm 外壳的后台自更新。

## 提示词

- **`search` 的描述**讲正面：什么时候用（每一步都用，不只第一步）、`pattern` 决定能找到什么、
  `query` 只在描述行为时给且要译成英文（用户多半说中文）、结果长什么样、空了放宽多了收窄。
- **`bash` 的描述**讲反面：别用 grep / rg / git grep 搜文本——代码归 `search`，
  其余文本归 `realontext query`。规矩写在违规发生的那个工具上，不往 system prompt 里加段落。

## 后果

- 没内置上（不支持的平台、configure 时下载失败）只出一条 CMake 警告，照样编译、照样起，
  退回 PATH 上的 realontext；都没有时 `search` 一调就报 `realontext not found`。
- 测试照 core 的做法把构建目录排进 PATH（`ENVIRONMENT_MODIFICATION`），测的是内置那一个。
- 文档、配置、日志、命令输出这类非代码文本不归 `search`，走 `bash` 里的 `realontext query`。
