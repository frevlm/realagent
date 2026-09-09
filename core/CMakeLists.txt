cmake_minimum_required(VERSION 3.30)
project(realagent-core CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(spdlog REQUIRED)
# macOS 的 libcurl 是系统库（SDK 自带头），FindCURL 会把 SDK usr/include 当 include dir
# 注入为 isystem——与 brew libc++ 头冲突。因此只取库路径，不用 CURL::libcurl target。
find_package(CURL REQUIRED)
# quiche（QUIC/HTTP3）：brew install cloudflare-quiche
find_path(QUICHE_INCLUDE_DIR quiche.h)
find_library(QUICHE_LIBRARY NAMES quiche)

# —— core 源码。三套协议各占 upstream/ 与 downstream/ 各一个文件（ADR-0017），
# llm.cpp 只放派发与共有部分；内置工具在 tools（ADR-0016：从前是 5 个动态库容器）——
add_library(realagent_core_lib STATIC
  src/config.cpp
  src/plugin.cpp
  src/llm/llm.cpp
  src/llm/upstream/anthropic_messages.cpp
  src/llm/upstream/openai_chat.cpp
  src/llm/upstream/openai_responses.cpp
  src/llm/downstream/anthropic_messages.cpp
  src/llm/downstream/openai_chat.cpp
  src/llm/downstream/openai_responses.cpp
  src/tools/tools.cpp
  src/tools/read.cpp
  src/tools/edit.cpp
  src/tools/bash.cpp
  src/tools/spawn.cpp
  src/tools/send_message.cpp
  src/agent/executor.cpp
  src/agent/approval.cpp
  src/agent/agent.cpp
  src/agent/verdict.cpp
  src/agent/agents.cpp
  src/agent/command.cpp
  src/agent/session.cpp
  src/agent/skills.cpp
  src/agent/commands.cpp
  src/agent/hooks.cpp
  src/agent/agent_defs.cpp
  src/agent/frontmatter.cpp
  src/mcp/client.cpp
  src/mcp/hub.cpp
)
target_include_directories(realagent_core_lib PUBLIC include)
target_link_libraries(realagent_core_lib PRIVATE ${CURL_LIBRARIES})

add_executable(realagent-core
  src/main.cpp
  src/server/quic_server.cpp
)
target_include_directories(realagent-core PRIVATE include ${QUICHE_INCLUDE_DIR})
target_link_libraries(realagent-core PRIVATE
  realagent_core_lib
  spdlog::spdlog
  ${CURL_LIBRARIES}
  ${QUICHE_LIBRARY}
)

enable_testing()
add_subdirectory(tests)
