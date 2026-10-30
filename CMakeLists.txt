cmake_minimum_required(VERSION 3.30)

# brew llvm clang 自带 SDK/头处理。禁用 CMake 的 macOS sysroot 注入，
# 否则会把 Xcode SDK usr/include 加为 isystem，与 libc++ 冲突（sys/_types.h 找不到 size_t）。
if(APPLE AND DEFINED CMAKE_CXX_COMPILER AND CMAKE_CXX_COMPILER MATCHES "llvm")
  set(CMAKE_OSX_SYSROOT "" CACHE PATH "Disable CMake sysroot injection (brew clang handles SDK)" FORCE)
endif()

project(realagent CXX)

enable_testing()

add_subdirectory(core)

# TUI（Go + Bubble Tea）：go build 产物 realagent-tui
find_program(GO_EXECUTABLE go)
if(GO_EXECUTABLE)
  add_custom_target(realagent-tui ALL
    COMMAND ${GO_EXECUTABLE} build -o ${CMAKE_BINARY_DIR}/realagent-tui ./cmd/realagent-tui
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tui
    COMMENT "Building realagent-tui (Go)"
  )
  # MCP 桥：别的 agent（Claude Code、Codex）通过它委派活儿给 realagent
  add_custom_target(realagent-mcp ALL
    COMMAND ${GO_EXECUTABLE} build -o ${CMAKE_BINARY_DIR}/realagent-mcp ./cmd/realagent-mcp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tui
    COMMENT "Building realagent-mcp (Go)"
  )
endif()
