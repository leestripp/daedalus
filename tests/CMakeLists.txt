cmake_minimum_required(VERSION 3.28)

# §Phase 11 P3 cleanup: doctest 2.4.13+ drops the <ciso646> include
# that GCC 15+ flags as deprecated.

# §Plan.md Phase 2: every test target compiles some src/core/*.cpp
# files directly (so the test can introspect internal helpers without
# linking the whole library). Those source files now reference
# `daedalus/paths.h` (the public Paths API). Without this include path
# the test compilation fails with "file not found".
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# doctest — fetched via CMake (matches the OmniNet pattern in the parent
# CMakeLists.txt) so the build is self-contained. The Readme still says
# "no doctest dependency" because doctest is the test framework, not a
# runtime dep; we fetch it into build/_deps/ like every other dep.
#
# ============================================================================
include(FetchContent)
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
)
FetchContent_MakeAvailable(doctest)

# Header-only — make it available to every test target defined below.
# `link_libraries` (directory property) appends doctest::doctest to all
# subsequent target_link_libraries() calls in this CMakeLists.txt, so
# tests that `#include <doctest/doctest.h>` Just Work.
link_libraries(doctest::doctest)

# ============================================================================
# Skill auto-discovery tests (C++23)
# ============================================================================

add_executable(daedalus_skill_auto_discovery_tests
    test_skill_auto_discovery.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SkillManager.cpp
    
    ${CMAKE_SOURCE_DIR}/src/core/SerperConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
)
target_include_directories(daedalus_skill_auto_discovery_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_skill_auto_discovery_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME skill_auto_discovery_tests COMMAND daedalus_skill_auto_discovery_tests)

# ============================================================================
# Regression: SettingsStore preserves all fields
# (previously omitted, causing user settings to be wiped on /models switch)
# ============================================================================


# ============================================================================
# Skill frontmatter parser tests (regression for std::regex segfault on
# large SKILL.md files, e.g. graphify at ~36 KB)
# ============================================================================

add_executable(daedalus_skill_frontmatter_tests
    test_skill_frontmatter.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SkillManager.cpp
    
    ${CMAKE_SOURCE_DIR}/src/core/SerperConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
)
target_include_directories(daedalus_skill_frontmatter_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_skill_frontmatter_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME skill_frontmatter_tests COMMAND daedalus_skill_frontmatter_tests)

# ============================================================================
# §Phase 15: per-server / per-skill enable/disable switches.
# Hermetic — no network, no API calls. Verifies the filter layer on
# McpServerManager and SkillManager end-to-end with synthetic SKILL.md
# files and a tiny servers.json.
# ============================================================================
add_executable(daedalus_phase15_disabled_switches_tests
    test_phase15_disabled_switches.cpp
)
target_include_directories(daedalus_phase15_disabled_switches_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(daedalus_phase15_disabled_switches_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME phase15_disabled_switches_tests
         COMMAND daedalus_phase15_disabled_switches_tests)

# ============================================================================
# §Phase 15: typo-detection warning for *_disabled lists.
# Verifies that Agent::initialize() emits a Warn log when a disabled
# name doesn't match any loaded server/skill. Uses a recording sink.
# ============================================================================
add_executable(daedalus_phase15_typo_warning_tests
    test_phase15_typo_warning.cpp
)
target_include_directories(daedalus_phase15_typo_warning_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(daedalus_phase15_typo_warning_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME phase15_typo_warning_tests
         COMMAND daedalus_phase15_typo_warning_tests)

# ============================================================================
# §Phase 14.7 (Q2, decided YES): per-project system_prompt override.
# Verifies that <project_root>/.daedalus/daedalus/system_prompt.txt is
# read during initialize() and appended to cfg.system_prompt (or used
# as-is if cfg.system_prompt is empty). Missing/empty file = silent no-op.
# ============================================================================
add_executable(daedalus_phase14_per_project_system_prompt_tests
    test_phase14_per_project_system_prompt.cpp
)
target_include_directories(daedalus_phase14_per_project_system_prompt_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(daedalus_phase14_per_project_system_prompt_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME phase14_per_project_system_prompt_tests
         COMMAND daedalus_phase14_per_project_system_prompt_tests)

# ============================================================================
# Serper.dev settings tests (regression for the "serper-search" key bug)
# ============================================================================

add_executable(daedalus_serper_settings_tests
    test_serper_settings.cpp
)
target_include_directories(daedalus_serper_settings_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_serper_settings_tests PRIVATE
    nlohmann_json::nlohmann_json
)
add_test(NAME serper_settings_tests COMMAND daedalus_serper_settings_tests)

# ============================================================================
# MCP HTTP-transport config tests (regression for the "Loaded 3 configured
# server(s)" bug — the user's apollocore entry was being silently dropped).
# ============================================================================
# ============================================================================
# §18.9 — McpProxiedTool: per-tool MCP adapter tests.

# ============================================================================
# §18.9 — McpProxiedTool: per-tool MCP adapter tests.
# ============================================================================
add_executable(daedalus_mcp_proxied_tool_tests
    test_mcp_proxied_tool.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/McpProxiedTool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashChainer.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashCommandExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Subprocess.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_proxied_tool_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mcp
    ${CMAKE_SOURCE_DIR}/src/tools
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_mcp_proxied_tool_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    OmniNet
    pthread
)
add_test(NAME mcp_proxied_tool_tests COMMAND daedalus_mcp_proxied_tool_tests)

# ============================================================================
# §18.9 — MCP Hub hardening tests (breaker, shutdown, listener plumbing).
# ============================================================================
add_executable(daedalus_mcp_hub_hardening_tests
    test_mcp_hub_hardening.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/McpProxiedTool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashChainer.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashCommandExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Subprocess.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_hub_hardening_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mcp
    ${CMAKE_SOURCE_DIR}/src/tools
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_mcp_hub_hardening_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    OmniNet
    pthread
)
add_test(NAME mcp_hub_hardening_tests COMMAND daedalus_mcp_hub_hardening_tests)

# ============================================================================
# Serper.dev STARTUP tests (behavioural: actually loads the user's settings
# file and verifies the serper-search key is configured and valid)
#
# Skippable in CI by setting DAEDALUS_SKIP_SERPER_STARTUP_TEST=1.
# ============================================================================

add_executable(daedalus_serper_startup_tests
    test_serper_startup.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SerperConfig.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_serper_startup_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_serper_startup_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
)
add_test(NAME serper_startup_tests COMMAND daedalus_serper_startup_tests)

# ============================================================================
# HttpSession tests (uses OmniNet HTTP, network may be unavailable)
# ============================================================================

add_executable(daedalus_http_session_tests
    test_http_session.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_http_session_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_BINARY_DIR}/_deps/omninet-src/src   # §NC-11: HttpSession.cpp #includes <OmniNet.h>
)
target_link_libraries(daedalus_http_session_tests PRIVATE
    OmniNet
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME http_session_tests COMMAND daedalus_http_session_tests)

# ============================================================================
# Model discovery tests (provider-agnostic /v1/models discovery)
# ============================================================================
add_executable(daedalus_model_discovery_tests
    test_model_discovery.cpp
)
target_include_directories(daedalus_model_discovery_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_model_discovery_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME model_discovery_tests COMMAND daedalus_model_discovery_tests)

# ============================================================================
# CurlHandle tests were removed 2026-07-16 as part of §NOMORE-CURL.
# CurlHandle.h is gone; the libcurl easy-handle RAII wrapper is no
# longer used. The OmniNet-backed HttpSession is the sole
# HTTP transport for the daemon.
# ============================================================================

# ============================================================================
# McpTransport tests (C++23)
# ============================================================================

add_executable(daedalus_mcp_transport_tests
    test_mcp_transport.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/StdioMcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_transport_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/mcp
    ${nlohmann_json_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/_deps/omninet-src/src   # §NC-11: HttpSession.cpp includes <OmniNet.h>
)
target_link_libraries(daedalus_mcp_transport_tests PRIVATE
    nlohmann_json::nlohmann_json
    OmniNet
    pthread
)
add_test(NAME mcp_transport_tests COMMAND daedalus_mcp_transport_tests)

# ============================================================================
# Metrics tests (C++23)
# ============================================================================

add_executable(daedalus_metrics_tests
    test_metrics.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Metrics.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_metrics_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
)
target_link_libraries(daedalus_metrics_tests PRIVATE
    pthread
)
add_test(NAME metrics_tests COMMAND daedalus_metrics_tests)

# ============================================================================
# ThreadPool tests - NO EXTERNAL DEPENDENCIES (C++23 std::jthread/stop_token)
# ============================================================================

# Create standalone ThreadPool test
add_executable(daedalus_thread_pool_tests
    test_thread_pool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_thread_pool_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
)
add_test(NAME thread_pool_tests COMMAND daedalus_thread_pool_tests)

# ============================================================================
# String utility tests - NO EXTERNAL DEPENDENCIES
# ============================================================================

# Create standalone string utility test (no external deps)
add_executable(daedalus_string_tests
    test_string_utils.cpp
)
target_include_directories(daedalus_string_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
)
add_test(NAME string_tests COMMAND daedalus_string_tests)

# ============================================================================
# UTF-8 tests - NO EXTERNAL DEPENDENCIES
# ============================================================================

add_executable(daedalus_utf8_tests
    test_utf8.cpp
)
target_include_directories(daedalus_utf8_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
)
add_test(NAME utf8_tests COMMAND daedalus_utf8_tests)

# ============================================================================
# Tests requiring nlohmann_json
# ============================================================================

find_package(nlohmann_json REQUIRED)
# §NC-11 (2026-07-16): find_package(CURL) removed. The entire daemon
# HTTP stack now uses OmniNet::HTTP::Client. No daemon code, header,
# or test links CURL::libcurl.

# Create JSON UTF-8 sanitization test
add_executable(daedalus_json_utf8_tests
    test_json_utf8.cpp
)
target_include_directories(daedalus_json_utf8_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_json_utf8_tests PRIVATE
    nlohmann_json::nlohmann_json
)
add_test(NAME json_utf8_tests COMMAND daedalus_json_utf8_tests)

# §NC-2 (2026-07-16): SSE parser test — pure header-only test that
# exercises the WHATWG SSE wire format. No external deps.
add_executable(daedalus_sse_parser_tests
    test_sse_parser.cpp
)
target_include_directories(daedalus_sse_parser_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_sse_parser_tests PRIVATE
    doctest::doctest
)
add_test(NAME sse_parser_tests COMMAND daedalus_sse_parser_tests)

# ============================================================================
# A2A client tests — hermetic; spins up an in-process OmniNet HTTP server.
# ============================================================================
add_executable(daedalus_a2a_basic_tests
    test_a2a_basic.cpp
)
target_include_directories(daedalus_a2a_basic_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/_deps/omninet-src/src   # §A2A: OmniNet HTTP server headers
)
target_link_libraries(daedalus_a2a_basic_tests PRIVATE
    doctest::doctest
    daedalus
    OmniNet
    pthread
)
add_test(NAME a2a_basic_tests COMMAND daedalus_a2a_basic_tests)

# ============================================================================
# A2A server tests — hermetic; spins up an in-process A2aServer and drives
# it through A2aClient.
# ============================================================================
add_executable(daedalus_a2a_server_tests
    test_a2a_server.cpp
)
target_include_directories(daedalus_a2a_server_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_a2a_server_tests PRIVATE
    doctest::doctest
    daedalus
    OmniNet
    pthread
)
add_test(NAME a2a_server_tests COMMAND daedalus_a2a_server_tests)


# §BUG-001 (2026-07-17): MiniMaxSseParser unit tests. Verifies the
# is_usage_only_chunk() gate logic that fixes the streaming-trailer
# bug (MiniMax sends `"usage": null` on the final chunk; old gate
# keyed on the usage field and threw away the recovered response).
#
# Links against the pre-built daedalus library (which provides the
# MiniMaxSseParser method bodies via LLMClient.cpp) rather than
# compiling LLMClient.cpp directly — that avoids pulling the full
# transitive include chain (SettingsStore → db/Database.h) into the
# test compilation unit.
add_executable(daedalus_minimax_sse_parser_tests
    test_minimax_sse_parser.cpp
)
target_include_directories(daedalus_minimax_sse_parser_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(daedalus_minimax_sse_parser_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
    daedalus
)
add_test(NAME minimax_sse_parser_tests COMMAND daedalus_minimax_sse_parser_tests)

# §TOKEN-COUNT-GRACEFUL (2026-07-17): Verifies the contract that
# missing/null/malformed `usage` in an LLM response does NOT crash
# the agent loop. The agent continues with a warning (per turn) and
# last_total_tokens_ remains 0. See docs/token_counts.md §
# "Graceful Degradation" and the comments in test_token_usage_graceful.cpp.
add_executable(daedalus_token_usage_graceful_tests
    test_token_usage_graceful.cpp
)
target_include_directories(daedalus_token_usage_graceful_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(daedalus_token_usage_graceful_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
)
add_test(NAME token_usage_graceful_tests COMMAND daedalus_token_usage_graceful_tests)

# Create Session Memory test
add_executable(daedalus_session_memory_tests
    test_session_memory.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SessionMemory.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_session_memory_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_session_memory_tests PRIVATE
    daedalus
    pthread
    nlohmann_json::nlohmann_json
)
add_test(NAME session_memory_tests COMMAND daedalus_session_memory_tests)

# Create BashTool test (standalone helpers test - no external deps)
add_executable(daedalus_bash_tool_tests
    test_bash_tool_helpers.cpp
)
target_include_directories(daedalus_bash_tool_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_bash_tool_tests PRIVATE
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME bash_tool_tests COMMAND daedalus_bash_tool_tests)

# Create BashPathChecker test
add_executable(daedalus_bash_path_checker_tests
    test_bash_path_checker.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_bash_path_checker_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/tools
)
add_test(NAME bash_path_checker_tests COMMAND daedalus_bash_path_checker_tests)

# BashTool background-mode integration test
add_executable(daedalus_bash_background_tests
    test_bash_background.cpp
)
target_include_directories(daedalus_bash_background_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_bash_background_tests PRIVATE
    daedalus
    pthread
    nlohmann_json::nlohmann_json
)
add_test(NAME bash_background_tests COMMAND daedalus_bash_background_tests)

# Create SystemInfoTool test
add_executable(daedalus_system_info_tests
    test_system_info.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/SystemInfoTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_system_info_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/tools
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/demos/example-tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_system_info_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
)
add_test(NAME system_info_tests COMMAND daedalus_system_info_tests)

# Create BackgroundProcessManager test
add_executable(daedalus_background_process_tests
    test_background_process.cpp
    ${CMAKE_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_background_process_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
)
add_test(NAME background_process_tests COMMAND daedalus_background_process_tests)

# Create McpClient test
add_executable(daedalus_mcp_client_tests
    test_mcp_client.cpp
)
target_include_directories(daedalus_mcp_client_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/mcp
)
add_test(NAME mcp_client_tests COMMAND daedalus_mcp_client_tests)

# Agent State Transitions test
add_executable(daedalus_agent_state_transition_tests
    test_agent_state_transitions.cpp
)
target_include_directories(daedalus_agent_state_transition_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_agent_state_transition_tests PRIVATE
    nlohmann_json::nlohmann_json
)
add_test(NAME agent_state_transition_tests COMMAND daedalus_agent_state_transition_tests)

# ============================================================================
# §State Machine: SetStatusTool unit tests
# ============================================================================

add_executable(daedalus_set_status_tool_tests
    test_set_status_tool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/SetStatusTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/library/AgentState.cpp
)
target_include_directories(daedalus_set_status_tool_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_set_status_tool_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
)
add_test(NAME set_status_tool_tests COMMAND daedalus_set_status_tool_tests)

# ============================================================================
# §Phase 8 acceptance tests — public API contract tests
# ============================================================================

# Two Agents coexisting in one process (Phase 2 contract: no singletons).
add_executable(daedalus_multi_instance_tests
    test_multi_instance.cpp
)
target_include_directories(daedalus_multi_instance_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_multi_instance_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME multi_instance_tests COMMAND daedalus_multi_instance_tests)

# AgentSink event contract (Phase 3: every event goes through the sink).
add_executable(daedalus_sink_events_tests
    test_sink_events.cpp
)
target_include_directories(daedalus_sink_events_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_sink_events_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME sink_events_tests COMMAND daedalus_sink_events_tests)

# §Phase C1: streaming tool-call progress events.
# Exercises BaseTool::execute(args, file_io, ToolProgressFn) end-to-end,
# plus the ToolHandler → AgentSink routing. See test_tool_progress.cpp.
add_executable(daedalus_tool_progress_tests
    test_tool_progress.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_tool_progress_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/tools
    ${CMAKE_SOURCE_DIR}/demos/example-tools
    ${CMAKE_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
    ${CMAKE_BINARY_DIR}/_deps/omninet-src/src
)
target_link_libraries(daedalus_tool_progress_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    OmniNet
    pthread
)
add_test(NAME tool_progress_tests COMMAND daedalus_tool_progress_tests)

# §Phase D1: AgentSink 3-arg overload (call_id propagation). Verifies
# the back-compat 2-arg path still fires correctly.
add_executable(daedalus_sink_call_id_tests
    test_sink_call_id.cpp
)
target_include_directories(daedalus_sink_call_id_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_sink_call_id_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME sink_call_id_tests COMMAND daedalus_sink_call_id_tests)

# Library does not call std::cin / std::cout / std::cerr (Phase 2.5: REPL
# belongs to the host, not the library).
add_executable(daedalus_no_repl_tests
    test_no_repl_in_library.cpp
)
target_include_directories(daedalus_no_repl_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_no_repl_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME no_repl_tests COMMAND daedalus_no_repl_tests)

# Per-Agent daedalus_home (Phase 8.5: two Agents in one process, each
# with its own settings dir).
add_executable(daedalus_per_agent_home_tests
    test_per_agent_home.cpp
)
target_include_directories(daedalus_per_agent_home_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_per_agent_home_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME per_agent_home_tests COMMAND daedalus_per_agent_home_tests)

# §MODEL-SWITCH: Agent::switch_model() clears conversation history and
# reconfigures the LLMClient for the new model's endpoint/context size.
add_executable(daedalus_switch_model_tests
    test_switch_model.cpp
)
target_include_directories(daedalus_switch_model_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_switch_model_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME switch_model_tests COMMAND daedalus_switch_model_tests)

# §CUSTOM-TOOLS: daedalus::BaseTool subclassing + Agent::register_tool().
# Hermetic — no network; exercises the public API end-to-end.
add_executable(daedalus_custom_tool_tests
    test_custom_tool.cpp
)
target_include_directories(daedalus_custom_tool_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_custom_tool_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME custom_tool_tests COMMAND daedalus_custom_tool_tests)

# Config / settings file location resolution (regression guard for the
# recurring "settings.json vs config.json vs settings/settings.json"
# bug family). See test_config_locations.cpp for what each pin covers.
add_executable(daedalus_config_locations_tests
    test_config_locations.cpp
)
target_include_directories(daedalus_config_locations_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_config_locations_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME config_locations_tests COMMAND daedalus_config_locations_tests)

# SettingsStore — SQLite-backed settings DB (Plan: Refactor settings to
# SQLite + public API). See test_settings_store.cpp for what each pin covers.
add_executable(daedalus_settings_store_tests
    test_settings_store.cpp
)
target_include_directories(daedalus_settings_store_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_settings_store_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME settings_store_tests COMMAND daedalus_settings_store_tests)

# End-to-end wire-in test: write a model via the public SettingsStore
# API, then verify Agent.list_models() sees it (this exercises
# SettingsStore is the primary store; AgentImpl wires directly to it.
add_executable(daedalus_settings_store_e2e_tests
    test_settings_store_e2e.cpp
)
target_include_directories(daedalus_settings_store_e2e_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_settings_store_e2e_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME settings_store_e2e_tests COMMAND daedalus_settings_store_e2e_tests)

# Agent::active_model() regression test: verifies the Agent can find
# models when using SettingsStore::instance() (default). Previously the
# webui demo set cfg.daedalus_home to a legacy path, causing AgentImpl
# to use SettingsStore::for_home() instead, which opened an empty DB.
add_executable(daedalus_settings_active_model_tests
    test_settings_active_model.cpp
)
target_include_directories(daedalus_settings_active_model_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_settings_active_model_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME settings_active_model_tests COMMAND daedalus_settings_active_model_tests)

# Regression test: SettingsStore::for_home() vs instance() distinction.
# Ensures that hosts understand these are different stores with different data.
add_executable(daedalus_settings_for_home_vs_instance_tests
    test_settings_for_home_vs_instance.cpp
)
target_include_directories(daedalus_settings_for_home_vs_instance_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_settings_for_home_vs_instance_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME settings_for_home_vs_instance_tests COMMAND daedalus_settings_for_home_vs_instance_tests)

# SettingsStore v0.5.0 mcp_servers table: tests CRUD operations.
add_executable(daedalus_mcp_servers_store_tests
    test_mcp_servers_store.cpp
)
target_include_directories(daedalus_mcp_servers_store_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_mcp_servers_store_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME mcp_servers_store_tests COMMAND daedalus_mcp_servers_store_tests)
add_executable(daedalus_mcp_load_from_store_tests
    test_mcp_load_from_store.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_load_from_store_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/mcp
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_mcp_load_from_store_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    OmniNet
    pthread
)
add_test(NAME mcp_load_from_store_tests COMMAND daedalus_mcp_load_from_store_tests)

# §FIX-MCP-STORE-LOAD (plan R3-P1, 2026-07-17): end-to-end regression
# — verify AgentImpl::AgentImpl actually loads MCP servers from the
# SettingsStore via load_from_store(). Previously this path was
# broken in TWO ways: (a) AgentImpl never called load_from_store(),
# only load_config(); (b) the whole MCP setup was inside
# `if (has_explicit_creds)`, so any user without a default model
# silently lost their MCP servers.
add_executable(daedalus_mcp_load_from_agent_tests
    test_mcp_load_from_agent.cpp
)
target_include_directories(daedalus_mcp_load_from_agent_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_mcp_load_from_agent_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME mcp_load_from_agent_tests COMMAND daedalus_mcp_load_from_agent_tests)

# Messages class tests
add_executable(daedalus_messages_tests
    test_messages.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Messages.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
)
target_include_directories(daedalus_messages_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_messages_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    pthread
)
add_test(NAME messages_tests COMMAND daedalus_messages_tests)

# §Phase 14: SystemPromptBuilder compose() tests (replaces the legacy
# test_system_prompt_builder.cpp which exercised the now-deleted identity /
# rules / workflow prose build path).
add_executable(daedalus_system_prompt_builder_tests
    test_system_prompt_compose.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SystemPromptBuilder.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_system_prompt_builder_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_system_prompt_builder_tests PRIVATE
    nlohmann_json::nlohmann_json
    pthread
)
# SYSTEM_PROMPT_BUILDER_CPP_PATH is consumed by the grep-guard test to
# confirm the library never re-introduces banned identity / workflow /
# tool-guidelines prose (see CodeMax.md §Phase 14 acceptance criteria).
target_compile_definitions(daedalus_system_prompt_builder_tests PRIVATE
    SYSTEM_PROMPT_BUILDER_CPP_PATH="${CMAKE_SOURCE_DIR}/src/core/SystemPromptBuilder.cpp"
)
add_test(NAME system_prompt_builder_tests COMMAND daedalus_system_prompt_builder_tests)

# §Phase 14: tests for the CLI demo's system_prompt resolution helper.
# Verifies the CLI flag override, the demo sidecar, and the per-project
# overlay are all wired correctly.
add_executable(daedalus_cli_system_prompt_tests
    test_cli_system_prompt.cpp
)
target_include_directories(daedalus_cli_system_prompt_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/demos/daedalus-cli
)
target_link_libraries(daedalus_cli_system_prompt_tests PRIVATE
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME cli_system_prompt_tests COMMAND daedalus_cli_system_prompt_tests)

# ToolDefinitions tests - verify get_tool_definitions() returns valid output
# Note: Includes PDFTool but with POPPLER include dirs for linking
add_executable(daedalus_tool_definitions_tests
    test_tool_definitions.cpp
    ${CMAKE_SOURCE_DIR}/src/api/HttpSession.cpp
    ${CMAKE_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/ToolHandler.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/ToolFactory.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashTool.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/GitTool.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/SystemInfoTool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/SkillTool.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/SearchTool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/MemoryTool.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/BackgroundTool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/UserTool.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/McpProxiedTool.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/PDFTool.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SkillManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${CMAKE_SOURCE_DIR}/src/core/SessionMemory.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${CMAKE_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashChainer.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${CMAKE_SOURCE_DIR}/src/tools/BashCommandExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Subprocess.cpp
)
target_include_directories(daedalus_tool_definitions_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/api
    ${CMAKE_SOURCE_DIR}/src/tools
    ${CMAKE_SOURCE_DIR}/src/mcp
    ${CMAKE_SOURCE_DIR}/demos/example-tools
    ${nlohmann_json_INCLUDE_DIR}
    ${POPPLER_INCLUDE_DIRS}
)
target_link_libraries(daedalus_tool_definitions_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    ${POPPLER_LIBRARIES}
    pthread
    OmniNet
)
add_test(NAME tool_definitions_tests COMMAND daedalus_tool_definitions_tests)

# ============================================================================
# OmniNet smoke test (P1 acceptance: OmniNet is fetchable, linkable, and its
# public API is reachable from daedalus code)
# ============================================================================

add_executable(daedalus_omninet_smoke_tests
    test_omninet_smoke.cpp
)
target_link_libraries(daedalus_omninet_smoke_tests PRIVATE
    OmniNet
    pthread
)
add_test(NAME omninet_smoke_tests COMMAND daedalus_omninet_smoke_tests)

# ============================================================================
# http_request tool tests (P2 acceptance: tool does HTTP/HTTPS via OmniNet)
# ============================================================================

add_executable(daedalus_http_request_tests
    test_http_request.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_http_request_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/demos/example-tools
)
target_link_libraries(daedalus_http_request_tests PRIVATE
    OmniNet
    pthread
)
add_test(NAME http_request_tests COMMAND daedalus_http_request_tests)

# ============================================================================
# Network diagnostic tool tests (P3 acceptance: dns_lookup, tcp_check,
# tls_inspect work end-to-end through OmniNet)
# ============================================================================

add_executable(daedalus_network_diagnostics_tests
    test_network_diagnostics.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_network_diagnostics_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/demos/example-tools
)
target_link_libraries(daedalus_network_diagnostics_tests PRIVATE
    OmniNet
    pthread
)
add_test(NAME network_diagnostics_tests COMMAND daedalus_network_diagnostics_tests)

# ============================================================================
# WebSocket tool tests (P4 acceptance: connect / send / recv / close
# via the WsRegistry)
# ============================================================================

add_executable(daedalus_websocket_tests
    test_websocket.cpp
    ${CMAKE_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${CMAKE_SOURCE_DIR}/src/core/FileIO.cpp
    ${CMAKE_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_websocket_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/demos/example-tools
)
target_link_libraries(daedalus_websocket_tests PRIVATE
    OmniNet
    pthread
)
add_test(NAME websocket_tests COMMAND daedalus_websocket_tests)

# ============================================================================
# Phase 1 (library refactor) public API smoke test
# Verifies include/daedalus/* compile, link, and run without any subsystem
# being wired yet (the stubs are intentional in Phase 1).
# ============================================================================

add_executable(daedalus_api_tests
    test_daedalus_api.cpp
)
target_include_directories(daedalus_api_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_api_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME daedalus_api_tests COMMAND daedalus_api_tests)

# ============================================================================
# §Phase 8.8: per-Agent agent_name namespace.
# Locks in the contract that:
#   - is_valid_agent_name() rejects empty escape attempts
#   - resolve_agent_dir() maps empty vs named correctly
#   - Two Agents / same project / different names don't collide
#   - empty agent_name → .daedalus/daedalus/ (no surprise)
#   - Agent::initialize() throws std::invalid_argument on a bad name
# ============================================================================

add_executable(daedalus_agent_name_namespace_tests
    test_agent_name_namespace.cpp
)
target_include_directories(daedalus_agent_name_namespace_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_agent_name_namespace_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME agent_name_namespace_tests COMMAND daedalus_agent_name_namespace_tests)

# ============================================================================
# §Phase 9.10: SSE streaming token count smoke test.
# Exercises one real LLM turn via daedalus-cli with model minimax-m2.7.
# Verifies:
#   1. SSE streaming produces assistant deltas (SSE pipeline works)
#   2. Turn completes without crash (state = Done)
#   3. Log messages are emitted (Warn about null usage is expected)
#   4. Either token counts > 0 OR Warn was emitted explaining why
# ============================================================================
add_executable(daedalus_token_counts_sse_tests
    test_token_counts_sse.cpp
)
target_include_directories(daedalus_token_counts_sse_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_token_counts_sse_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME daedalus_token_counts_sse_tests COMMAND daedalus_token_counts_sse_tests)
set_tests_properties(daedalus_token_counts_sse_tests PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "live-llm"
)
# §Phase 14: this test is skipped unless DAEDALUS_RUN_LIVE_LLM_TESTS=1.
# It makes a real MiniMax API call; see test_token_counts_sse.cpp.

# ============================================================================
# daedalus_no_content_duplicate_tests — Phase 9.11 regression test
# Verifies that assistant text is emitted EXACTLY ONCE via
# `on_assistant_delta` during SSE streaming.  Bug: icarus host reported
# (see prompt.txt) that text appeared twice — once streamed live, then
# again at line 2029 when the synthesized message's full content was
# re-emitted.
#
# The test asks the model to output a unique sentinel string and asserts
# it appears exactly once in the concatenated delta buffer.
# ============================================================================
add_executable(daedalus_no_content_duplicate_tests
    test_no_content_duplicate.cpp
)
target_include_directories(daedalus_no_content_duplicate_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_no_content_duplicate_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME daedalus_no_content_duplicate_tests COMMAND daedalus_no_content_duplicate_tests)
set_tests_properties(daedalus_no_content_duplicate_tests PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "live-llm"
)
# §Phase 14: skipped unless DAEDALUS_RUN_LIVE_LLM_TESTS=1 (above).

# ============================================================================
# Plan.md Phase 1: XDG-compliant paths singleton + env-var resolution.
# Tests Paths::global() singleton, set_override(), and all 6 env vars
# from Plan.md §3.7 (DAEDALUS_HOME / XDG_CONFIG_HOME / XDG_DATA_HOME /
# XDG_STATE_HOME / XDG_CACHE_HOME / DAEDALUS_SYSTEM_*_DIRS).
# ============================================================================
add_executable(daedalus_xdg_paths_tests
    test_xdg_paths.cpp
)
target_include_directories(daedalus_xdg_paths_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_xdg_paths_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME xdg_paths_tests COMMAND daedalus_xdg_paths_tests)

# ============================================================================
# Plan.md Phase 1: ProjectPaths — per-project, per-agent namespace.
# Verifies the cardinal rule from Plan.md §1.1: two Agent instances in
# the same project with different agent_name values get different
# agent_dir() (and therefore different memory_dir() / memory_file()).
# ============================================================================
add_executable(daedalus_project_paths_tests
    test_project_paths.cpp
)
target_include_directories(daedalus_project_paths_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_project_paths_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME project_paths_tests COMMAND daedalus_project_paths_tests)

# ============================================================================
# Plan.md Phase 1: Two-tier enforcement — no XDG-layer memory writes.
# Static grep over src/ plus runtime checks that memory_dir() never
# resolves under user_data_root(). If a future commit reintroduces the
# "global memory" bug, this test fails.
# ============================================================================
add_executable(daedalus_two_tier_rule_tests
    test_two_tier_rule.cpp
)
target_include_directories(daedalus_two_tier_rule_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_two_tier_rule_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME two_tier_rule_tests COMMAND daedalus_two_tier_rule_tests)

# ============================================================================
# Plan.md Phase 4: legacy ~/.daedalus/ deprecation warning.
# ============================================================================
# Plan.md Phase 5: --migrate-data subcommand (logic-level tests).
# Plan.md Phase 5: --migrate-data subcommand (logic-level tests).
# Tests the MigratePlan/apply functions from demos/daedalus-cli/migrate.h
# directly. The CLI wrapper is one line of plumbing.
# ============================================================================
add_executable(daedalus_migrate_data_tests
    test_migrate_data.cpp
    ${CMAKE_SOURCE_DIR}/demos/daedalus-cli/migrate.cpp
)
target_include_directories(daedalus_migrate_data_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/demos/daedalus-cli
)
target_link_libraries(daedalus_migrate_data_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME migrate_data_tests
    COMMAND daedalus_migrate_data_tests)

# ============================================================================
# Plan.md §5.1 mitigation: ensure_directories() creates the four XDG
# user-tier roots with the correct permissions (config/data=0750,
# state/cache=0700). Idempotent. Honours env-var overrides.
# ============================================================================
add_executable(daedalus_ensure_directories_tests
    test_ensure_directories.cpp
)
target_include_directories(daedalus_ensure_directories_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_ensure_directories_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME ensure_directories_tests
    COMMAND daedalus_ensure_directories_tests)

# ============================================================================
# §FIX-PLAN-R3-REAL (2026-07-18): REAL end-to-end test that uses the
# canonical `Testing/test-cli-bare/project/sum.cpp` fixture to drive
# the actual library code paths through the public API.
# ============================================================================
add_executable(daedalus_real_world_tools_tests
    test_real_world_tools.cpp
)
target_include_directories(daedalus_real_world_tools_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(daedalus_real_world_tools_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME real_world_tools_tests COMMAND daedalus_real_world_tools_tests)
set_tests_properties(real_world_tools_tests PROPERTIES
    LABELS "real-world"
)

# ============================================================================
# §FIX-PLAN-R3-DEMOS (2026-07-18): Hello-world smoke test for the 4 demo
# apps. Each demo binary is launched with --version; we verify exit 0
# and the canonical banner. We also compile + run a trivial C++
# "Hello, World!" to prove the toolchain still produces a working
# binary. The test is hermetic — no network, no SettingsStore writes.
# Per-demo gating (so a default-config build with webui=OFF still
# passes): the test consults $DAEDALUS_DEMO_<NAME>_OPT env vars that
# ctest sets from CMake's option cache at run time.
# ============================================================================
add_executable(daedalus_demos_hello_world_tests
    test_demos_hello_world.cpp
)
add_test(NAME demos_hello_world_tests COMMAND daedalus_demos_hello_world_tests)
set(_dhw_cli  "$<IF:$<BOOL:${DAEDALUS_BUILD_DEMOS}>,1,0>")
set(_dhw_sub  "$<IF:$<BOOL:${DAEDALUS_DEMO_SUBAGENT}>,1,0>")
set(_dhw_mgr  "$<IF:$<BOOL:${DAEDALUS_DEMO_MANAGER}>,1,0>")
set(_dhw_web  "$<IF:$<BOOL:${DAEDALUS_DEMO_WEBUI}>,1,0>")
set_tests_properties(demos_hello_world_tests PROPERTIES
    ENVIRONMENT "DAEDALUS_DEMO_CLI_OPT=${_dhw_cli};DAEDALUS_DEMO_SUBAGENT_OPT=${_dhw_sub};DAEDALUS_DEMO_MANAGER_OPT=${_dhw_mgr};DAEDALUS_DEMO_WEBUI_OPT=${_dhw_web}"
    LABELS "smoke"
)

# ============================================================================
# §Phase E1: JS-side wiring check for the webui tool-panel.
#
# Two Node-based tests live under tests/webui/:
#
#   1. three_call_test.js — pure-JS data-flow test using a vm context
#      and a minimal DOM mock. Runs in ~30ms with no external deps.
#
#   2. playwright_three_call_test.js — full browser test that drives
#      a real headless Chromium. Adds computed-CSS verification on
#      top of the data-flow checks. Requires `npm install playwright`
#      + `npx playwright install chromium` (~290 MiB).
#
# Neither is registered with add_test() because they require
# Node.js >= 18, which the project's CMake doesn't enforce.
# Run manually from the tests/webui/ directory:
#
#   node three_call_test.js
#   node playwright_three_call_test.js
#   npm test                # runs both in sequence
#
# When the project grows a node-based test runner (e.g. CI with
# Playwright preinstalled), both files should be wired into ctest.
# ============================================================================
