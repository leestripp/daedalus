cmake_minimum_required(VERSION 3.28)

# §Phase 11 P3 cleanup: doctest 2.4.13+ drops the <ciso646> include
# that GCC 15+ flags as deprecated.

# §Plan.md Phase 2: every test target compiles some src/core/*.cpp
# files directly (so the test can introspect internal helpers without
# linking the whole library). Those source files now reference
# `daedalus/paths.h` (the public Paths API). Without this include path
# the test compilation fails with "file not found".
include_directories(${PROJECT_SOURCE_DIR}/include)

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
    ${PROJECT_SOURCE_DIR}/src/core/SkillManager.cpp
    
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
)
target_include_directories(daedalus_skill_auto_discovery_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_skill_auto_discovery_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME skill_auto_discovery_tests COMMAND daedalus_skill_auto_discovery_tests)

# ============================================================================
# §SECOND-BRAIN: Global Second Brain (markdown KB + FTS5 index, schema v10)
# ============================================================================
add_executable(daedalus_second_brain_tests
    test_second_brain.cpp
)
target_include_directories(daedalus_second_brain_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_second_brain_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME second_brain_tests COMMAND daedalus_second_brain_tests)

find_package(SQLite3 REQUIRED)
add_executable(daedalus_second_brain_concurrency_tests
    test_second_brain_concurrency.cpp
)
target_include_directories(daedalus_second_brain_concurrency_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_second_brain_concurrency_tests PRIVATE
    daedalus
    SQLite::SQLite3
    pthread
)
add_test(NAME second_brain_concurrency_tests
    COMMAND daedalus_second_brain_concurrency_tests)
set_tests_properties(second_brain_concurrency_tests PROPERTIES TIMEOUT 120)

add_executable(daedalus_brain_tool_tests
    test_brain_tool.cpp
)
target_include_directories(daedalus_brain_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_brain_tool_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME brain_tool_tests COMMAND daedalus_brain_tool_tests)

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
    ${PROJECT_SOURCE_DIR}/src/core/SkillManager.cpp
    
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
)
target_include_directories(daedalus_skill_frontmatter_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
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
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_phase15_disabled_switches_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME phase15_disabled_switches_tests
         COMMAND daedalus_phase15_disabled_switches_tests)

# ============================================================================
# §Phase 16: runtime enable/disable for skills, MCP servers, and tools.
# Exercises the public Agent API end-to-end.
# ============================================================================
add_executable(daedalus_runtime_enable_disable_tests
    test_runtime_enable_disable.cpp
)
target_include_directories(daedalus_runtime_enable_disable_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_runtime_enable_disable_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME runtime_enable_disable_tests
         COMMAND daedalus_runtime_enable_disable_tests)

# ============================================================================
# §Phase 15: typo-detection warning for *_disabled lists.
# Verifies that Agent::initialize() emits a Warn log when a disabled
# name doesn't match any loaded server/skill. Uses a recording sink.
# ============================================================================
add_executable(daedalus_phase15_typo_warning_tests
    test_phase15_typo_warning.cpp
)
target_include_directories(daedalus_phase15_typo_warning_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_phase15_typo_warning_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME phase15_typo_warning_tests
         COMMAND daedalus_phase15_typo_warning_tests)

# ============================================================================
# §SKILL-ORDER regression: AgentImpl::initialize() must attach the per-agent
# SkillManager to the LLMClient (the client is constructed before the
# manager exists, so the constructor always received nullptr). Hermetic —
# dummy creds force client construction without any network access.
# ============================================================================
add_executable(daedalus_skill_manager_wiring_tests
    test_skill_manager_wiring.cpp
)
target_include_directories(daedalus_skill_manager_wiring_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_skill_manager_wiring_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME skill_manager_wiring_tests
         COMMAND daedalus_skill_manager_wiring_tests)

# ============================================================================
# daedalus_skill_discovery_refresh_tests
#
# Regression test for the 2026-09-17 skill-discovery bug (bug_report.md):
# discover_skills served the session-start cache forever, the `directory`
# argument was silently ignored, and the project-local `.agents/skills/`
# default was resolved against the daemon CWD instead of project_root.
# Hermetic — temp dirs only, no network.
# ============================================================================
add_executable(daedalus_skill_discovery_refresh_tests
    test_skill_discovery_refresh.cpp
)
target_include_directories(daedalus_skill_discovery_refresh_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_skill_discovery_refresh_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME skill_discovery_refresh_tests
         COMMAND daedalus_skill_discovery_refresh_tests)

# ============================================================================
# daedalus_mcp_servers_json_fallback_tests
#
# Regression test for BUG-001 (2026-09-17, MonkeyByte report): MCP tools
# were never injected because the documented mcp/servers.json fallback
# silently no-opped after the v0.5.0 SQLite migration, and the agent had
# no MCP introspection tool. Hermetic — temp dirs only, no spawns.
# ============================================================================
add_executable(daedalus_mcp_servers_json_fallback_tests
    test_mcp_servers_json_fallback.cpp
)
target_include_directories(daedalus_mcp_servers_json_fallback_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_mcp_servers_json_fallback_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME mcp_servers_json_fallback_tests
         COMMAND daedalus_mcp_servers_json_fallback_tests)

# ============================================================================
# daedalus_vault_tests
#
# §TASK-2026-09-21-01: SecretsVault encrypted password manager — crypto
# round-trip, tamper/wrong-key detection, no-plaintext-in-DB, migration.
# Hermetic — temp dirs only, no network, no spawns.
# ============================================================================
add_executable(daedalus_vault_tests
    test_vault.cpp
)
target_include_directories(daedalus_vault_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_vault_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME vault_tests
         COMMAND daedalus_vault_tests)

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
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_phase14_per_project_system_prompt_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME phase14_per_project_system_prompt_tests
         COMMAND daedalus_phase14_per_project_system_prompt_tests)

# ============================================================================

# ============================================================================

# ============================================================================
# daedalus_list_tools_tests
#
# §LIST-TOOLS-CORE (2026-09-21): list_tools tool — live tool roster,
# filter/verbose params, description shortening. Hermetic.
# ============================================================================
add_executable(daedalus_list_tools_tests
    test_list_tools.cpp
)
target_include_directories(daedalus_list_tools_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_list_tools_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME list_tools_tests
         COMMAND daedalus_list_tools_tests)

# ============================================================================
# daedalus_mcp_control_tests
#
# §MCP-CONTROL-CORE (2026-09-22): mcp_control tool — agent-driven MCP
# server list/enable/disable. Hermetic: standalone McpServerManager,
# no connections, no spawns.
# ============================================================================
add_executable(daedalus_mcp_control_tests
    test_mcp_control.cpp
)
target_include_directories(daedalus_mcp_control_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_mcp_control_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME mcp_control_tests
         COMMAND daedalus_mcp_control_tests)

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
    ${PROJECT_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/McpProxiedTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashChainer.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashCommandExecutor.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Subprocess.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_proxied_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/mcp
    ${PROJECT_SOURCE_DIR}/src/tools
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/core
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
# §SUBPROC-EOF-HANG / §SUBPROC-DRAIN-HANG regression tests
# (daedalus_bugs.md BUG 2, MonkeyByte 2026-09-01): execute_combined must
# honour its timeout even when the child closes its inherited fds (EOF
# branch) or a setsid grandchild keeps the pipe write ends open (timeout
# drain branch).
# ============================================================================
add_executable(daedalus_subprocess_timeout_tests
    test_subprocess_timeout.cpp
)
target_include_directories(daedalus_subprocess_timeout_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_subprocess_timeout_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME subprocess_timeout_tests COMMAND daedalus_subprocess_timeout_tests)

# ============================================================================
# §18.9 — MCP Hub hardening tests (breaker, shutdown, listener plumbing).
# ============================================================================
add_executable(daedalus_mcp_hub_hardening_tests
    test_mcp_hub_hardening.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/McpProxiedTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashChainer.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashCommandExecutor.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Subprocess.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_hub_hardening_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/mcp
    ${PROJECT_SOURCE_DIR}/src/tools
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/core
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

# ============================================================================
# HttpSession tests (uses OmniNet HTTP, network may be unavailable)
# ============================================================================

add_executable(daedalus_http_session_tests
    test_http_session.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_http_session_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${OmniNet_SOURCE_DIR}/src   # §NC-11: HttpSession.cpp #includes <OmniNet.h>
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/src/mcp/StdioMcpTransport.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_transport_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/mcp
    ${nlohmann_json_INCLUDE_DIR}
    ${OmniNet_SOURCE_DIR}/src   # §NC-11: HttpSession.cpp includes <OmniNet.h>
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
    ${PROJECT_SOURCE_DIR}/src/core/Metrics.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_metrics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
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
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_thread_pool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
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
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
)
add_test(NAME string_tests COMMAND daedalus_string_tests)

# ============================================================================
# UTF-8 tests - NO EXTERNAL DEPENDENCIES
# ============================================================================

add_executable(daedalus_utf8_tests
    test_utf8.cpp
)
target_include_directories(daedalus_utf8_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
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
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
    ${OmniNet_SOURCE_DIR}/src   # §A2A: OmniNet HTTP server headers
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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_a2a_server_tests PRIVATE
    doctest::doctest
    daedalus
    OmniNet
    pthread
)
add_test(NAME a2a_server_tests COMMAND daedalus_a2a_server_tests)

# ============================================================================
# A2A Python SDK smoke test — runs daedalus-cli --a2a-server and probes it
# with the official Google A2A Python SDK. Requires scripts/.venv installed.
#
# The wrapper script skips gracefully when no models are configured, so a
# clean checkout without a provider/model setup will not fail the build.
# ============================================================================
add_custom_target(a2a_smoke_test
    COMMAND ${PROJECT_SOURCE_DIR}/scripts/run_a2a_smoke_test.sh $<TARGET_FILE:daedalus-cli>
    DEPENDS daedalus-cli
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    COMMENT "Running A2A Python SDK smoke test (skips if no models configured)..."
    VERBATIM
)


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
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_minimax_sse_parser_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
    daedalus
)
add_test(NAME minimax_sse_parser_tests COMMAND daedalus_minimax_sse_parser_tests)

# §ThinkTagFilter (2026-08-01): unit tests for the stream-safe inline
# <think>...</think> stripper (src/api/ThinkTagFilter.h). Header-only,
# but links daedalus for consistency with the other parser tests.
add_executable(daedalus_think_tag_filter_tests
    test_think_tag_filter.cpp
)
target_include_directories(daedalus_think_tag_filter_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_think_tag_filter_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
    daedalus
)
add_test(NAME think_tag_filter_tests COMMAND daedalus_think_tag_filter_tests)

# §AutoCompress (2026-08-01): end-to-end proof that the 90% auto-compress
# loop fires AgentSink::on_compaction through the public Agent API, driven
# against an in-process fake LLM server (see test_auto_compress.cpp).
add_executable(daedalus_auto_compress_tests
    test_auto_compress.cpp
)
target_include_directories(daedalus_auto_compress_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_auto_compress_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
    daedalus
    pthread
)
add_test(NAME auto_compress_tests COMMAND daedalus_auto_compress_tests)

# §TOKEN-COUNT-GRACEFUL (2026-07-17): Verifies the contract that
# missing/null/malformed `usage` in an LLM response does NOT crash
# the agent loop. The agent continues with a warning (per turn) and
# last_total_tokens_ remains 0. See docs/sse_streaming.md §
# "Token-count graceful degradation" and the comments in test_token_usage_graceful.cpp.
add_executable(daedalus_token_usage_graceful_tests
    test_token_usage_graceful.cpp
)
target_include_directories(daedalus_token_usage_graceful_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_token_usage_graceful_tests PRIVATE
    doctest::doctest
    nlohmann_json::nlohmann_json
)
add_test(NAME token_usage_graceful_tests COMMAND daedalus_token_usage_graceful_tests)

# Create Session Memory test
add_executable(daedalus_session_memory_tests
    test_session_memory.cpp
    ${PROJECT_SOURCE_DIR}/src/core/SessionMemory.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_session_memory_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/tools
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
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/tools
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
    ${PROJECT_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_bash_path_checker_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
)
add_test(NAME bash_path_checker_tests COMMAND daedalus_bash_path_checker_tests)

# BashTool background-mode integration test
add_executable(daedalus_bash_background_tests
    test_bash_background.cpp
)
target_include_directories(daedalus_bash_background_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_bash_background_tests PRIVATE
    daedalus
    pthread
    nlohmann_json::nlohmann_json
)
add_test(NAME bash_background_tests COMMAND daedalus_bash_background_tests)

# BashTool output sanitization tests (TTY control-sequence stripping)
add_executable(daedalus_bash_output_sanitize_tests
    test_bash_output_sanitize.cpp
)
target_include_directories(daedalus_bash_output_sanitize_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
)
target_link_libraries(daedalus_bash_output_sanitize_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME bash_output_sanitize_tests COMMAND daedalus_bash_output_sanitize_tests)

# Create SystemInfoTool test
add_executable(daedalus_system_info_tests
    test_system_info.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/SystemInfoTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_system_info_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/demos/example-tools
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
    ${PROJECT_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_background_process_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
)
add_test(NAME background_process_tests COMMAND daedalus_background_process_tests)

# End-to-end execute_bash completion routing through the public Agent API.
add_executable(daedalus_background_completion_tests
    test_background_completion.cpp
)
target_include_directories(daedalus_background_completion_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
)
target_link_libraries(daedalus_background_completion_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME background_completion_tests COMMAND daedalus_background_completion_tests)

# Persistent one-shot/recurring scheduler, tool CRUD, and shutdown behavior.
add_executable(daedalus_task_scheduler_tests
    test_task_scheduler.cpp
)
target_include_directories(daedalus_task_scheduler_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_task_scheduler_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME task_scheduler_tests COMMAND daedalus_task_scheduler_tests)
set_tests_properties(task_scheduler_tests PROPERTIES TIMEOUT 20)

add_executable(daedalus_schedule_task_tool_tests
    test_schedule_task_tool.cpp
)
target_include_directories(daedalus_schedule_task_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_schedule_task_tool_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME schedule_task_tool_tests COMMAND daedalus_schedule_task_tool_tests)

# Create McpClient test
add_executable(daedalus_mcp_client_tests
    test_mcp_client.cpp
)
target_include_directories(daedalus_mcp_client_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/mcp
)
add_test(NAME mcp_client_tests COMMAND daedalus_mcp_client_tests)

# Agent State Transitions test
add_executable(daedalus_agent_state_transition_tests
    test_agent_state_transitions.cpp
)
target_include_directories(daedalus_agent_state_transition_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
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
    ${PROJECT_SOURCE_DIR}/src/tools/SetStatusTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/library/AgentState.cpp
)
target_include_directories(daedalus_set_status_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_set_status_tool_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
)
add_test(NAME set_status_tool_tests COMMAND daedalus_set_status_tool_tests)

# ============================================================================
# §State Machine: LLMClient set_status → state-change routing tests
# ============================================================================

add_executable(daedalus_state_change_routing_tests
    test_state_change_routing.cpp
)
target_include_directories(daedalus_state_change_routing_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_state_change_routing_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME state_change_routing_tests COMMAND daedalus_state_change_routing_tests)

# ============================================================================
# Todo list tools tests
# ============================================================================

add_executable(daedalus_todo_list_tool_tests
    test_todo_list_tool.cpp
)
target_include_directories(daedalus_todo_list_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_todo_list_tool_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME todo_list_tool_tests COMMAND daedalus_todo_list_tool_tests)

# ============================================================================
# Public Agent todo API tests
# ============================================================================

add_executable(daedalus_todo_public_api_tests
    test_todo_public_api.cpp
)
target_include_directories(daedalus_todo_public_api_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_todo_public_api_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME todo_public_api_tests COMMAND daedalus_todo_public_api_tests)

# ============================================================================
# §Phase 8 acceptance tests — public API contract tests
# ============================================================================

# Two Agents coexisting in one process (Phase 2 contract: no singletons).
add_executable(daedalus_multi_instance_tests
    test_multi_instance.cpp
)
target_include_directories(daedalus_multi_instance_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_sink_events_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME sink_events_tests COMMAND daedalus_sink_events_tests)

# §BUG-005 regression: cancel() at idle must not poison the next turn.
add_executable(daedalus_cancel_stale_flag_tests
    test_cancel_stale_flag.cpp
)
target_include_directories(daedalus_cancel_stale_flag_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_cancel_stale_flag_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME cancel_stale_flag_tests COMMAND daedalus_cancel_stale_flag_tests)

# §Phase C1: streaming tool-call progress events.
# Exercises BaseTool::execute(args, file_io, ToolProgressFn) end-to-end,
# plus the ToolHandler → AgentSink routing. See test_tool_progress.cpp.
add_executable(daedalus_tool_progress_tests
    test_tool_progress.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_tool_progress_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/tools
    ${PROJECT_SOURCE_DIR}/demos/example-tools
    ${PROJECT_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
    ${OmniNet_SOURCE_DIR}/src
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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_sink_call_id_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME sink_call_id_tests COMMAND daedalus_sink_call_id_tests)

# ============================================================================
# LLM lifecycle callbacks: on_llm_wait_started / on_llm_response_started /
# on_llm_request_finished. Hermetic portion runs always; live-LLM portion
# is skipped unless DAEDALUS_RUN_LIVE_LLM_TESTS=1.
# ============================================================================
add_executable(daedalus_llm_lifecycle_callbacks_tests
    test_llm_lifecycle_callbacks.cpp
)
target_include_directories(daedalus_llm_lifecycle_callbacks_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_llm_lifecycle_callbacks_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME llm_lifecycle_callbacks_tests COMMAND daedalus_llm_lifecycle_callbacks_tests)
set_tests_properties(llm_lifecycle_callbacks_tests PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "live-llm"
    TIMEOUT 300
)

# Library does not call std::cin / std::cout / std::cerr (Phase 2.5: REPL
# belongs to the host, not the library).
add_executable(daedalus_no_repl_tests
    test_no_repl_in_library.cpp
)
target_include_directories(daedalus_no_repl_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_switch_model_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME switch_model_tests COMMAND daedalus_switch_model_tests)

# Agent::delete_model() — public-API passthrough to
# SettingsStore::delete_model(). Hermetic — no network.
add_executable(daedalus_delete_model_tests
    test_delete_model.cpp
)
target_include_directories(daedalus_delete_model_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_delete_model_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME delete_model_tests COMMAND daedalus_delete_model_tests)

# §CUSTOM-TOOLS: daedalus::BaseTool subclassing + Agent::register_tool().
# Hermetic — no network; exercises the public API end-to-end.
add_executable(daedalus_custom_tool_tests
    test_custom_tool.cpp
)
target_include_directories(daedalus_custom_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_mcp_servers_store_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME mcp_servers_store_tests COMMAND daedalus_mcp_servers_store_tests)

# §ProviderPlugins Phase 1: ProviderRegistry behavior.
add_executable(daedalus_provider_plugin_registry_tests
    test_provider_plugin_registry.cpp
)
target_include_directories(daedalus_provider_plugin_registry_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_provider_plugin_registry_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME provider_plugin_registry_tests COMMAND daedalus_provider_plugin_registry_tests)

# §ProviderPlugins Phase 2: providers.plugin column + v4->v5 migration.
add_executable(daedalus_provider_settings_migration_tests
    test_provider_settings_migration.cpp
)
target_include_directories(daedalus_provider_settings_migration_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_provider_settings_migration_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME provider_settings_migration_tests COMMAND daedalus_provider_settings_migration_tests)

# §ProviderPlugins Phase 3: MiniMaxPlugin hooks.
add_executable(daedalus_provider_minimax_plugin_tests
    test_provider_minimax_plugin.cpp
)
target_include_directories(daedalus_provider_minimax_plugin_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_provider_minimax_plugin_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME provider_minimax_plugin_tests COMMAND daedalus_provider_minimax_plugin_tests)

# §ProviderPlugins Phase 4: MoonshotPlugin hooks.
add_executable(daedalus_provider_moonshot_plugin_tests
    test_provider_moonshot_plugin.cpp
)
target_include_directories(daedalus_provider_moonshot_plugin_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_provider_moonshot_plugin_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME provider_moonshot_plugin_tests COMMAND daedalus_provider_moonshot_plugin_tests)

# §ProviderPlugins Phase 5: LmStudioPlugin hooks.
add_executable(daedalus_provider_lmstudio_plugin_tests
    test_provider_lmstudio_plugin.cpp
)
target_include_directories(daedalus_provider_lmstudio_plugin_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_provider_lmstudio_plugin_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME provider_lmstudio_plugin_tests COMMAND daedalus_provider_lmstudio_plugin_tests)

# §ThinkingLevel: user-selectable thinking levels (Moonshot effort +
# on/off, MiniMax M3 on/off, LmStudio stripping, capability matrix).
add_executable(daedalus_thinking_levels_tests
    test_thinking_levels.cpp
)
target_include_directories(daedalus_thinking_levels_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_thinking_levels_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME thinking_levels_tests COMMAND daedalus_thinking_levels_tests)

# §ContextCache: prompt-cache usage parsing + cache-safe status message.
add_executable(daedalus_context_cache_tests
    test_context_cache.cpp
)
target_include_directories(daedalus_context_cache_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_context_cache_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME context_cache_tests COMMAND daedalus_context_cache_tests)

add_executable(daedalus_mcp_load_from_store_tests
    test_mcp_load_from_store.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_mcp_load_from_store_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/mcp
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/core
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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_mcp_load_from_agent_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME mcp_load_from_agent_tests COMMAND daedalus_mcp_load_from_agent_tests)

# Messages class tests
add_executable(daedalus_messages_tests
    test_messages.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Messages.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
)
target_include_directories(daedalus_messages_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_messages_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    pthread
)
add_test(NAME messages_tests COMMAND daedalus_messages_tests)

# §MSG-HEADERS (2026-10-04): message origin envelope — stamp / wire_render
add_executable(daedalus_message_headers_tests
    test_message_headers.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Messages.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
)
target_include_directories(daedalus_message_headers_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_message_headers_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    pthread
)
add_test(NAME message_headers_tests COMMAND daedalus_message_headers_tests)

# §fix (2026-07-30): position-aware chain validity tests (sanitize,
# guarded tool-result injection, truncation block retention)
add_executable(daedalus_message_chain_validity_tests
    test_message_chain_validity.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Messages.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
)
target_include_directories(daedalus_message_chain_validity_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_message_chain_validity_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json

    pthread
)
add_test(NAME message_chain_validity_tests COMMAND daedalus_message_chain_validity_tests)

# §Phase 14: SystemPromptBuilder compose() tests (replaces the legacy
# test_system_prompt_builder.cpp which exercised the now-deleted identity /
# rules / workflow prose build path).
add_executable(daedalus_system_prompt_builder_tests
    test_system_prompt_compose.cpp
    ${PROJECT_SOURCE_DIR}/src/core/SystemPromptBuilder.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_system_prompt_builder_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
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
    SYSTEM_PROMPT_BUILDER_CPP_PATH="${PROJECT_SOURCE_DIR}/src/core/SystemPromptBuilder.cpp"
)
add_test(NAME system_prompt_builder_tests COMMAND daedalus_system_prompt_builder_tests)

# §Phase 14: tests for the CLI demo's system_prompt resolution helper.
# Verifies the CLI flag override, the demo sidecar, and the per-project
# overlay are all wired correctly.
add_executable(daedalus_cli_system_prompt_tests
    test_cli_system_prompt.cpp
)
target_include_directories(daedalus_cli_system_prompt_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/demos/daedalus-cli
)
target_link_libraries(daedalus_cli_system_prompt_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME cli_system_prompt_tests COMMAND daedalus_cli_system_prompt_tests)

# ToolDefinitions tests - verify get_tool_definitions() returns valid output
# Note: PDFTool is core again (§PDF-CORE); compiled here directly
# like the other tool sources, with POPPLER include dirs for linking.
add_executable(daedalus_tool_definitions_tests
    test_tool_definitions.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
    ${PROJECT_SOURCE_DIR}/src/core/ThreadPool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ToolHandler.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/ToolFactory.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashTool.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/GitTool.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/SystemInfoTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/SkillTool.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/SearchTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/MemoryTool.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/BackgroundTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/UserTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/McpProxiedTool.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/PDFTool.cpp
    ${PROJECT_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/SkillManager.cpp
    ${PROJECT_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${PROJECT_SOURCE_DIR}/src/core/SessionMemory.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpClient.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/McpServerManager.cpp
    ${PROJECT_SOURCE_DIR}/src/mcp/HttpMcpTransport.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashChainer.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashPathChecker.cpp
    ${PROJECT_SOURCE_DIR}/src/tools/BashCommandExecutor.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Subprocess.cpp
)
target_include_directories(daedalus_tool_definitions_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${PROJECT_SOURCE_DIR}/src/tools
    ${PROJECT_SOURCE_DIR}/src/mcp
    ${PROJECT_SOURCE_DIR}/demos/example-tools
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
    ${PROJECT_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_http_request_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/demos/example-tools
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
    ${PROJECT_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_network_diagnostics_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/demos/example-tools
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
    ${PROJECT_SOURCE_DIR}/demos/example-tools/NetworkTool.cpp
    ${PROJECT_SOURCE_DIR}/src/core/FileIO.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
)
target_include_directories(daedalus_websocket_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/demos/example-tools
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_agent_name_namespace_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME agent_name_namespace_tests COMMAND daedalus_agent_name_namespace_tests)

# ============================================================================
# Pending queue management: list / edit / remove / clear queued user messages
# (Agent::pending_messages, edit_pending_message, remove_pending_message,
# clear_pending_messages).
# ============================================================================

add_executable(daedalus_pending_messages_tests
    test_pending_messages.cpp
)
target_include_directories(daedalus_pending_messages_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_pending_messages_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME pending_messages_tests COMMAND daedalus_pending_messages_tests)

# ============================================================================
# MonkeyByte upstream report 2026-09-10 (vs d73d1ba): QUEUE-MUTEX (BUG-3/
# REQ-4 cross-thread queue + cancel), TURN-ERROR (BUG-4 terminal-error
# query API), COMPRESS-RESULT (REQ-3 NoModel), STATECHANGE-TYPED (REQ-5
# typed on_state_change with legacy int forwarding).
# ============================================================================

add_executable(daedalus_upstream_2026_09_10_tests
    test_upstream_2026_09_10.cpp
)
target_include_directories(daedalus_upstream_2026_09_10_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_upstream_2026_09_10_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME upstream_2026_09_10_tests COMMAND daedalus_upstream_2026_09_10_tests)

# ============================================================================
# §HISTORY-SNAPSHOT (REQ-2): Messages::history_snapshot() projection —
# text roles round-trip, tool plumbing elided, multipart flattened.
# ============================================================================

add_executable(daedalus_history_snapshot_tests
    test_history_snapshot.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Messages.cpp
    ${PROJECT_SOURCE_DIR}/src/core/Logger.cpp
    ${PROJECT_SOURCE_DIR}/src/core/BackgroundProcessManager.cpp
    ${PROJECT_SOURCE_DIR}/src/api/HttpSession.cpp
)
target_include_directories(daedalus_history_snapshot_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/core
    ${PROJECT_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_history_snapshot_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME history_snapshot_tests COMMAND daedalus_history_snapshot_tests)

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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_token_counts_sse_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME daedalus_token_counts_sse_tests COMMAND daedalus_token_counts_sse_tests)
set_tests_properties(daedalus_token_counts_sse_tests PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "live-llm"
    TIMEOUT 300
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
    ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(daedalus_no_content_duplicate_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME daedalus_no_content_duplicate_tests COMMAND daedalus_no_content_duplicate_tests)
set_tests_properties(daedalus_no_content_duplicate_tests PROPERTIES
    SKIP_RETURN_CODE 77
    LABELS "live-llm"
    TIMEOUT 300
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/demos/daedalus-cli/migrate.cpp
)
target_include_directories(daedalus_migrate_data_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/demos/daedalus-cli
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
    ${PROJECT_SOURCE_DIR}/include
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
    ${PROJECT_SOURCE_DIR}/include
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

# ============================================================================
# daedalus-settings-tui smoke test: the FTXUI settings editor must boot
# without a TTY, open a fresh settings DB, and dump every section with
# exit code 0. XDG_DATA_HOME redirects the store to a hermetic tmp dir.
# ============================================================================
if(DAEDALUS_DEMO_SETTINGS_TUI)
    add_test(NAME settings_tui_dump
             COMMAND daedalus-settings-tui --dump)
    set_tests_properties(settings_tui_dump PROPERTIES
        ENVIRONMENT "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/settings_tui_home"
        LABELS "smoke"
    )
endif()

# ============================================================================
# §on_error: AgentSink::on_error structured error channel + ErrorKind
# mapping (HTTP status / provider error-type) unit tests
# ============================================================================
add_executable(daedalus_on_error_tests
    test_on_error_callback.cpp
)
target_include_directories(daedalus_on_error_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/api
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_on_error_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME on_error_callback_tests COMMAND daedalus_on_error_tests)

# ============================================================================
# §logging: Logger self-initialization (cwd fallback), explicit project
# init, and level filtering unit tests
# ============================================================================
add_executable(daedalus_logger_tests
    test_logger.cpp
)
target_include_directories(daedalus_logger_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(daedalus_logger_tests PRIVATE
    daedalus
    pthread
)
add_test(NAME logger_tests COMMAND daedalus_logger_tests)

# ============================================================================
# §BUG-006/007/008: tool-approval control — authoritative auto_approve
# config, unconditional ToolHandler flag application, approval-cache
# host controls, and on_command_auto_approved sink visibility.
# ============================================================================
add_executable(daedalus_approval_control_tests
    test_approval_control.cpp
)
target_include_directories(daedalus_approval_control_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_approval_control_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME approval_control_tests COMMAND daedalus_approval_control_tests)

# ============================================================================
# daedalus_bugs.md §1: ToolFactory::register_tool() replace-by-name dedup
# (duplicate function definitions → provider HTTP 400) + unregister_tool().
# ============================================================================
add_executable(daedalus_tool_factory_dedup_tests
    test_tool_factory_dedup.cpp
)
target_include_directories(daedalus_tool_factory_dedup_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_tool_factory_dedup_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME tool_factory_dedup_tests COMMAND daedalus_tool_factory_dedup_tests)

# ============================================================================
# §READ-IMAGE-CORE (2026-08-10): core read_image tool — PNG scaled path,
# jpeg/gif/bmp/webp passthrough, SVG rejection, webp magic-byte detection.
# ============================================================================
add_executable(daedalus_read_image_tool_tests
    test_read_image_tool.cpp
)
target_include_directories(daedalus_read_image_tool_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
    ${JPEG_INCLUDE_DIRS}
)
target_link_libraries(daedalus_read_image_tool_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    ${JPEG_LIBRARIES}
    pthread
)
add_test(NAME read_image_tool_tests COMMAND daedalus_read_image_tool_tests)

# ============================================================================
# Host image attachments (0.10.0 bug: ![alt](path) never reached the model).
# Markdown + queue_image_attachments -> payload-only injection, vision gate.
# ============================================================================
add_executable(daedalus_image_attachment_tests
    test_image_attachments.cpp
)
target_include_directories(daedalus_image_attachment_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_image_attachment_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME image_attachment_tests COMMAND daedalus_image_attachment_tests)

# ============================================================================
# daedalus_bugs.md §2: fatal API error circuit breaker — alternating 400
# error shapes must end the turn after 2 cycles instead of looping and
# flooding the history with [SYSTEM ERROR] messages. Drives LLMClient
# against an in-process HTTP server.
# ============================================================================
add_executable(daedalus_fatal_breaker_tests
    test_fatal_breaker.cpp
)
target_include_directories(daedalus_fatal_breaker_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_fatal_breaker_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME fatal_breaker_tests COMMAND daedalus_fatal_breaker_tests)

# ============================================================================
# §STATUS-KIND-BREAKER regression tests (2026-10-25): Done is an intentional
# terminal signal; other states are UI-only. Three consecutive status-only
# iterations of the SAME non-terminal kind suppress a runaway, while mixed
# kinds continue normally. Drives LLMClient against an in-process HTTP server.
# ============================================================================
add_executable(daedalus_status_only_breaker_tests
    test_status_only_breaker.cpp
)
target_include_directories(daedalus_status_only_breaker_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_status_only_breaker_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME status_only_breaker_tests COMMAND daedalus_status_only_breaker_tests)

# §STEER-MESSAGE: prove cross-thread guidance lands only after a complete
# multi-tool result block, using a gated in-process OpenAI-compatible server.
add_executable(daedalus_steer_message_tests
    test_steer_messages.cpp
)
target_include_directories(daedalus_steer_message_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/include
    ${nlohmann_json_INCLUDE_DIR}
)
target_link_libraries(daedalus_steer_message_tests PRIVATE
    daedalus
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME steer_message_tests COMMAND daedalus_steer_message_tests)

# ============================================================================
# §Monorepo (2026-09-24): label every daedalus test "lib" so the superbuild
# root can offer `ctest -L lib` slices. Must happen here — test names are
# only visible to set_property in the directory that created them.
# ============================================================================
get_property(_daedalus_all_tests DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY TESTS)
if(_daedalus_all_tests)
    set_property(TEST ${_daedalus_all_tests} APPEND PROPERTY LABELS lib)
endif()

# ============================================================================
# §WEB-SEARCH-CORE (2026-10-10): unified web_search provider registry
# (parsers, failover chain, schema v8 migration, ToolFactory roster)
# ============================================================================

add_executable(daedalus_web_search_tests
    test_web_search.cpp
)
target_include_directories(daedalus_web_search_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${nlohmann_json_INCLUDE_DIR}
    ${OmniNet_SOURCE_DIR}/src
)
target_link_libraries(daedalus_web_search_tests PRIVATE
    daedalus
    OmniNet
    nlohmann_json::nlohmann_json
    pthread
)
add_test(NAME web_search_tests COMMAND daedalus_web_search_tests)

# ============================================================================
# §WEB-SEARCH-CORE (2026-10-10): unified web_search provider registry —
# parsers (canned fixtures), failover chain (injected fakes), schema v8
# migration, ToolFactory roster. Hermetic: no network.
# ============================================================================

# Never let an intermittent test deadlock block the suite indefinitely.
# Preserve intentional longer timeouts (concurrency, scheduler, live LLM).
get_property(_daedalus_all_tests DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY TESTS)
foreach(_test IN LISTS _daedalus_all_tests)
    get_property(_timeout TEST ${_test} PROPERTY TIMEOUT)
    if(NOT _timeout)
        set_property(TEST ${_test} PROPERTY TIMEOUT 60)
    endif()
endforeach()
