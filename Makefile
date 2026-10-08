# realagent 顶层开发入口 —— 只做一件事：把常用命令转交给 CMake/Ninja。
# 不重复声明构建规则，避免与 CMakeLists.txt 双源真相。

BUILD_DIR := build
NINJA     := $(BUILD_DIR)/build.ninja
CORE      := $(BUILD_DIR)/core/realagent-core
TUI       := $(BUILD_DIR)/realagent-tui

# 首次运行自动 configure（生成器固定 Ninja）；已配置过则空操作
$(NINJA):
	cmake -S . -B $(BUILD_DIR) -G Ninja

.PHONY: all core tui gui dev dev-browser dev-app test run tui-run fmt fmt-check clean help

all: core tui        ## 构建全部（core + TUI），默认目标

core: $(NINJA)       ## 构建 core（C++ WebSocket 服务）
	cmake --build $(BUILD_DIR) --target realagent-core

tui: $(NINJA)        ## 构建 TUI（Go + Bubble Tea）
	cmake --build $(BUILD_DIR) --target realagent-tui

# gui 不进 all：要 Node 与 Wails 两套工具链，只动 core / TUI 的人不该被它拖着
WAILS := $(shell go env GOPATH)/bin/wails

gui:                 ## 构建 gui（Wails：TS 网页 + Go 壳），产物在 gui/build/bin/
	cd gui && $(WAILS) build

# 后台起 core，前台跑 $(1)，它退出时收掉 core。用启动日志当就绪信号。
# 注意：recipe 里不能写 shell 注释——续行会让注释吞掉整条命令。
define with-core
	@$(CORE) > $(BUILD_DIR)/core.log 2>&1 & \
	core_pid=$$!; \
	trap 'kill $$core_pid 2>/dev/null' INT TERM; \
	for i in $$(seq 1 50); do grep -q "运行在 127.0.0.1" $(BUILD_DIR)/core.log 2>/dev/null && break; kill -0 $$core_pid 2>/dev/null || break; sleep 0.1; done; \
	$(1); \
	rc=$$?; \
	kill $$core_pid 2>/dev/null; \
	wait $$core_pid 2>/dev/null; \
	exit $$rc
endef

dev: all             ## 开发：core + TUI
	$(call with-core,$(TUI))

dev-browser: core    ## 开发：core + gui 网页（浏览器开 localhost:34115，热重载）
	$(call with-core,cd gui && REALAGENT_WORKDIR=$(CURDIR) $(WAILS) dev -browser)

dev-app: core        ## 开发：core + gui 桌面窗口（热重载）
	$(call with-core,cd gui && REALAGENT_WORKDIR=$(CURDIR) $(WAILS) dev)

# 先构建全部目标（含测试可执行文件）再跑 ctest。只 configure 不构建的话，
# 干净的 build 目录里根本没有测试二进制，ctest 会把四个用例全报 "Not Run"——
# 那是"没跑"，不是"通过"，而退出码长得跟真失败一样。
test: $(NINJA)       ## 构建全部目标（含测试）并运行 CTest
	cmake --build $(BUILD_DIR)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

run: core            ## 启动 core 服务（127.0.0.1:12345）
	$(CORE)

tui-run: tui         ## 启动 TUI 客户端
	$(TUI)

# 格式化的单一真相是 .clang-format。Ctrl+S(clangd)、AI 改动后的 hook、
# pre-commit 校验全读同一份，三处不会打架。
fmt:                              ## 按 .clang-format 就地格式化 core 的 C++ 源码
	scripts/fmt.sh

fmt-check:           ## 只校验格式，不改文件（pre-commit 走的也是这条）
	scripts/fmt.sh --check

clean:               ## 清空构建产物
	rm -rf $(BUILD_DIR)

help:                ## 列出所有目标
	@grep -E '^[a-z-]+:.*##' $(MAKEFILE_LIST) | sed -E 's/:.*## / | /' | awk -F'|' '{printf "  %-12s %s\n", $$1, $$2}'
