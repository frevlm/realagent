# 每个测试链同一个 realagent_core_lib，不再各自列源码。
# mcp_stub_server 是 MCP 测试的对手进程。
add_executable(mcp_stub_server mcp_stub_server.cpp)
target_include_directories(mcp_stub_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

foreach(name config llm context_session session skills tools agent verdict stop_loop mcp mcp_hub commands hooks plugin_e2e)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE realagent_core_lib)
  target_compile_definitions(test_${name} PRIVATE MCP_STUB_SERVER="$<TARGET_FILE:mcp_stub_server>")
  add_dependencies(test_${name} mcp_stub_server)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# context_session 的端到端要两个假 revlm 当对手：h2c 的（revlm::net 那条路）与只认
# HTTP/1.1 的 httplib（退回 curl 那条路）
target_link_libraries(test_context_session PRIVATE httplib::httplib revlm::net_test_server)

# 测试二进制不在 realagent-core 旁边，PATH 照 core 启动时那样补上内置的 realontext
set_tests_properties(tools PROPERTIES
  ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${CMAKE_CURRENT_BINARY_DIR}/..")
