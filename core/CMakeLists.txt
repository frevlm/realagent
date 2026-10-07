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
  src/tools/search.cpp
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

# realontext（search 工具背后的那条命令）随 core 一起发：npm 上那个平台包里的原生二进制，
# 钉死版本、校验 sha512，放在 realagent-core 旁边。core 启动时把自己所在目录排进 PATH 最前，
# search 与 bash 里的 `realontext` 找到的就是这一个（ADR-0027）。
set(REALONTEXT_VERSION 0.16.0)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(REALONTEXT_PLATFORM darwin-arm64)
  set(REALONTEXT_SHA512 2852777048195f70aa3169eda8b40148092322f2ad07b459afb5e55c8ddc9a0ad54652c824c3c0bfd260a149229a759cfc2496c59c1a66d0754bd072c51a85a9)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(REALONTEXT_PLATFORM linux-x64)
  set(REALONTEXT_SHA512 169c6a21f898ad0750d910c6079c16f1f857808aaff405612d58c3ead9f1a3c9b485db0c89472692f977a8be52e32e92835039e85bff68a06c69db62678cb718)
endif()
set(REALONTEXT_BIN ${CMAKE_CURRENT_BINARY_DIR}/realontext)
if(REALONTEXT_PLATFORM AND NOT EXISTS ${REALONTEXT_BIN})
  set(_tgz ${CMAKE_CURRENT_BINARY_DIR}/realontext-${REALONTEXT_PLATFORM}-${REALONTEXT_VERSION}.tgz)
  file(DOWNLOAD
    https://registry.npmjs.org/@realontext/${REALONTEXT_PLATFORM}/-/${REALONTEXT_PLATFORM}-${REALONTEXT_VERSION}.tgz
    ${_tgz} EXPECTED_HASH SHA512=${REALONTEXT_SHA512} STATUS _st)
  list(GET _st 0 _rc)
  if(_rc EQUAL 0)
    file(ARCHIVE_EXTRACT INPUT ${_tgz} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/realontext-pkg)
    file(COPY_FILE ${CMAKE_CURRENT_BINARY_DIR}/realontext-pkg/package/bin/realontext ${REALONTEXT_BIN})
    file(REMOVE_RECURSE ${_tgz} ${CMAKE_CURRENT_BINARY_DIR}/realontext-pkg)
  endif()
endif()
if(NOT EXISTS ${REALONTEXT_BIN})
  message(WARNING "realontext 没内置上（平台 ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR} 或下载失败）：search 工具退回 PATH 上的 realontext")
endif()

enable_testing()
add_subdirectory(tests)
