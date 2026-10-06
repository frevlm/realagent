cmake_minimum_required(VERSION 3.30)
project(realagent-core CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# macOS 的 libcurl 是系统库（SDK 自带头），FindCURL 会把 SDK usr/include 当 include dir
# 注入为 isystem——与 brew libc++ 头冲突。因此只取库路径，不用 CURL::libcurl target。
find_package(CURL REQUIRED)
# cpp-httplib（WebSocket 服务端）：header-only，钉一个版本，与 realontext 的 Relay 同。
# 只在本机回环上说话：TLS 与压缩都不要，对外的 TLS 留给前面的反向代理。
include(FetchContent)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_TEST OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
  GIT_TAG v0.57.1
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(httplib)
# 普通 -I，排在每个 -isystem 之前：Homebrew 在 /opt/homebrew/include 装着另一个版本的 httplib.h，不许它先被找到
set_property(TARGET httplib PROPERTY INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "")

add_library(realagent_core_lib STATIC
  src/config.cpp
  src/plugin.cpp
  src/proc.cpp
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
  src/agent/hooks.cpp
  src/agent/catalog.cpp
  src/mcp/client.cpp
  src/mcp/hub.cpp
)
target_include_directories(realagent_core_lib PUBLIC include)
target_link_libraries(realagent_core_lib PRIVATE ${CURL_LIBRARIES})

add_executable(realagent-core
  src/main.cpp
  src/server/server.cpp
)
target_include_directories(realagent-core PRIVATE include)
target_link_libraries(realagent-core PRIVATE
  realagent_core_lib
  ${CURL_LIBRARIES}
  httplib::httplib
)

enable_testing()
add_subdirectory(tests)
