# 每个测试链同一个 realagent_core_lib，不再各自列源码。
# mcp_stub_server 是 MCP 测试的对手进程。
add_executable(mcp_stub_server mcp_stub_server.cpp)
target_include_directories(mcp_stub_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

foreach(name config llm session skills tools agent verdict stop_loop mcp mcp_hub commands hooks plugin_e2e)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE realagent_core_lib)
  target_compile_definitions(test_${name} PRIVATE MCP_STUB_SERVER="$<TARGET_FILE:mcp_stub_server>")
  add_dependencies(test_${name} mcp_stub_server)
  add_test(NAME ${name} COMMAND test_${name})
endforeach()
