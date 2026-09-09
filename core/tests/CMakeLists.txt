# 配置与模型档位测试（不起服务、不碰网络）
add_executable(test_config test_config.cpp ../src/config.cpp)
target_include_directories(test_config PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME config COMMAND test_config)

# llm 模块测试（三套协议各自的造请求与解析 / 计价 / 端点配置校验）
add_executable(test_llm test_llm.cpp
  ../src/llm/llm.cpp
  ../src/llm/upstream/anthropic_messages.cpp
  ../src/llm/upstream/openai_chat.cpp
  ../src/llm/upstream/openai_responses.cpp
  ../src/llm/downstream/anthropic_messages.cpp
  ../src/llm/downstream/openai_chat.cpp
  ../src/llm/downstream/openai_responses.cpp
  ../src/config.cpp)
target_include_directories(test_llm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME llm COMMAND test_llm)

# 会话持久化与事件帧回放测试（JSONL 读写 + to_frames 转换；cwd 指到临时目录）
add_executable(test_session test_session.cpp ../src/agent/session.cpp ../src/config.cpp)
target_include_directories(test_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME session COMMAND test_session)

# skill 扫盘与 frontmatter 解析（ADR-0022：两处来源、近的覆盖远的、坏的跳过）
add_executable(test_skills test_skills.cpp ../src/agent/skills.cpp ../src/agent/frontmatter.cpp ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_skills PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME skills COMMAND test_skills)

# 内置工具 + 权限裁决测试（ADR-0016 真正换掉的那半边：静态工具表 + permission 三值）
add_executable(test_tools test_tools.cpp
                          ../src/tools/tools.cpp ../src/tools/read.cpp ../src/tools/edit.cpp
                          ../src/tools/bash.cpp ../src/tools/spawn.cpp ../src/tools/send_message.cpp ../src/tools/stop.cpp
                          ../src/agent/executor.cpp ../src/agent/hooks.cpp
                          ../src/agent/agents.cpp ../src/agent/agent.cpp ../src/agent/agent_defs.cpp ../src/agent/command.cpp ../src/agent/commands.cpp ../src/agent/session.cpp
                          ../src/agent/skills.cpp ../src/agent/frontmatter.cpp
                          ../src/mcp/client.cpp ../src/mcp/hub.cpp
                          ../src/llm/llm.cpp
                          ../src/llm/upstream/anthropic_messages.cpp ../src/llm/upstream/openai_chat.cpp
                          ../src/llm/upstream/openai_responses.cpp
                          ../src/llm/downstream/anthropic_messages.cpp
                          ../src/llm/downstream/openai_chat.cpp
                          ../src/llm/downstream/openai_responses.cpp
                          ../src/agent/approval.cpp ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_tools PRIVATE ${CURL_LIBRARIES})
target_compile_definitions(test_tools PRIVATE MCP_STUB_SERVER="$<TARGET_FILE:mcp_stub_server>")
add_dependencies(test_tools mcp_stub_server)
add_test(NAME tools COMMAND test_tools)

# agent 的收件箱与线程生命周期（ADR-0019）。端点没配，LLM 调用当场失败——
# 这里验的是消息进出收件箱，不是模型回了什么，所以不碰网络。
add_executable(test_agent test_agent.cpp
  ../src/agent/agent.cpp ../src/agent/agent_defs.cpp ../src/agent/command.cpp ../src/agent/commands.cpp ../src/agent/agents.cpp ../src/agent/executor.cpp ../src/agent/hooks.cpp ../src/agent/approval.cpp
  ../src/agent/session.cpp ../src/agent/skills.cpp ../src/agent/frontmatter.cpp ../src/llm/llm.cpp
  ../src/mcp/client.cpp ../src/mcp/hub.cpp
  ../src/tools/tools.cpp ../src/tools/read.cpp ../src/tools/edit.cpp
  ../src/tools/bash.cpp ../src/tools/spawn.cpp ../src/tools/send_message.cpp ../src/tools/stop.cpp
  ../src/llm/upstream/anthropic_messages.cpp ../src/llm/upstream/openai_chat.cpp
  ../src/llm/upstream/openai_responses.cpp ../src/llm/downstream/anthropic_messages.cpp
  ../src/llm/downstream/openai_chat.cpp ../src/llm/downstream/openai_responses.cpp
  ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_agent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_agent PRIVATE ${CURL_LIBRARIES})
add_test(NAME agent COMMAND test_agent)

# MCP 客户端（ADR-0023）。对手是同目录下的 stub server：官方参考 server 实测是旧纪元的
# （server/discover 回 -32601），而我们只说 2026-07-28，所以现成的对手一个都用不了。
add_executable(mcp_stub_server mcp_stub_server.cpp)
target_include_directories(mcp_stub_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

add_executable(test_mcp test_mcp.cpp ../src/mcp/client.cpp)
target_include_directories(test_mcp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(test_mcp PRIVATE MCP_STUB_SERVER="$<TARGET_FILE:mcp_stub_server>")
add_dependencies(test_mcp mcp_stub_server)
add_test(NAME mcp COMMAND test_mcp)

add_executable(test_mcp_hub test_mcp_hub.cpp ../src/mcp/hub.cpp ../src/mcp/client.cpp ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_mcp_hub PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_definitions(test_mcp_hub PRIVATE MCP_STUB_SERVER="$<TARGET_FILE:mcp_stub_server>")
add_dependencies(test_mcp_hub mcp_stub_server)
add_test(NAME mcp_hub COMMAND test_mcp_hub)

# prompt 命令：扫盘、前缀、内置不可覆盖、$ARGUMENTS（ADR-0024 §7）
add_executable(test_commands test_commands.cpp ../src/agent/commands.cpp
               ../src/agent/command.cpp ../src/agent/agent.cpp ../src/agent/agent_defs.cpp ../src/agent/agents.cpp
               ../src/agent/executor.cpp ../src/agent/hooks.cpp ../src/agent/approval.cpp ../src/agent/session.cpp
               ../src/agent/skills.cpp ../src/agent/frontmatter.cpp
               ../src/tools/tools.cpp ../src/tools/read.cpp ../src/tools/edit.cpp
               ../src/tools/bash.cpp ../src/tools/spawn.cpp ../src/tools/send_message.cpp
               ../src/tools/stop.cpp
               ../src/mcp/client.cpp ../src/mcp/hub.cpp ../src/llm/llm.cpp
               ../src/llm/upstream/anthropic_messages.cpp ../src/llm/upstream/openai_chat.cpp
               ../src/llm/upstream/openai_responses.cpp
               ../src/llm/downstream/anthropic_messages.cpp ../src/llm/downstream/openai_chat.cpp
               ../src/llm/downstream/openai_responses.cpp
               ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_commands PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_commands PRIVATE ${CURL_LIBRARIES})
add_test(NAME commands COMMAND test_commands)

# hook：扫盘、执行、deny 只收紧、超时不卡死（ADR-0024 §6）
add_executable(test_hooks test_hooks.cpp ../src/agent/hooks.cpp ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_hooks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
add_test(NAME hooks COMMAND test_hooks)

# 端到端：一个照抄 Claude Code 布局的 plugin，五样成员一次吃下（ADR-0024）
add_executable(test_plugin_e2e test_plugin_e2e.cpp
               ../src/agent/skills.cpp ../src/agent/commands.cpp ../src/agent/command.cpp
               ../src/agent/agent_defs.cpp ../src/agent/hooks.cpp ../src/agent/frontmatter.cpp
               ../src/agent/agent.cpp ../src/agent/agents.cpp ../src/agent/executor.cpp
               ../src/agent/approval.cpp ../src/agent/session.cpp
               ../src/tools/tools.cpp ../src/tools/read.cpp ../src/tools/edit.cpp
               ../src/tools/bash.cpp ../src/tools/spawn.cpp ../src/tools/send_message.cpp
               ../src/tools/stop.cpp
               ../src/mcp/client.cpp ../src/mcp/hub.cpp ../src/llm/llm.cpp
               ../src/llm/upstream/anthropic_messages.cpp ../src/llm/upstream/openai_chat.cpp
               ../src/llm/upstream/openai_responses.cpp
               ../src/llm/downstream/anthropic_messages.cpp ../src/llm/downstream/openai_chat.cpp
               ../src/llm/downstream/openai_responses.cpp
               ../src/config.cpp ../src/plugin.cpp)
target_include_directories(test_plugin_e2e PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_plugin_e2e PRIVATE ${CURL_LIBRARIES})
add_test(NAME plugin_e2e COMMAND test_plugin_e2e)
