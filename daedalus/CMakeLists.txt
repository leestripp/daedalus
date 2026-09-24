cmake_minimum_required(VERSION 3.28)

# ============================================================================
# Compiler selection: rely on the system default (gcc/g++ on Debian/Ubuntu).
# Users can still force a specific toolchain with `CC=… CXX=… cmake -S . -B build`
# — e.g. `CC=clang CXX=clang++` or `CC=gcc-13 CXX=g++-13`.
# Nothing is pinned here so the distro's default compiler wins, matching the
# pre-§Phase 9.9 behaviour.
# ============================================================================

project(daedalus LANGUAGES CXX C)

# Detect whether daedalus is being built stand-alone or consumed as a
# dependency via add_subdirectory() / FetchContent. Several defaults below
# change depending on this. §Monorepo (2026-09-24): the superbuild root sets
# DAEDALUS_SUPERBUILD so a top-level monorepo build behaves exactly like a
# standalone library build.
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR OR DAEDALUS_SUPERBUILD)
    set(DAEDALUS_IS_TOP_LEVEL ON)
else()
    set(DAEDALUS_IS_TOP_LEVEL OFF)
endif()

# Emit compile_commands.json so any IDE / static-analysis tool (clangd,
# cppcheck, …) can map source files to their exact compiler invocation.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Automatic version from git tags
# §Monorepo (2026-09-24): ONE version stream for the whole repo — bare
# x.y.z tags at the root cover the library AND both hosts. No prefixed
# per-component tags; every component describes the same tag set.
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "v0.0.0")
endif()

# Parse version into components (e.g., v1.2.3 or 1.2.3 -> 1, 2, 3)
# Git describe format: [tag]-[commits]-g[hash][-dirty]
# Example: v0.2.0-1-g34ae062-dirty or 0.3.0-1-g34ae062-dirty

# First, extract just the tag part (everything before the first - that's after digits)
# Match: optional v/V, then three dot-separated numbers
string(REGEX MATCHALL "[0-9]+" VERSION_PARTS "${GIT_VERSION}")
# VERSION_PARTS will be a list like "0;3;0;1;34ae062" - we want first 3

list(LENGTH VERSION_PARTS PARTS_LEN)
if(PARTS_LEN GREATER_EQUAL 3)
    list(GET VERSION_PARTS 0 VERSION_MAJOR)
    list(GET VERSION_PARTS 1 VERSION_MINOR)
    list(GET VERSION_PARTS 2 VERSION_PATCH)
    
    # Build VERSION_STRING - use the original tag format with suffix
    # Find the version prefix in GIT_VERSION
    string(REGEX MATCH "^[vV]?[0-9]+\\.[0-9]+\\.[0-9]+" VERSION_TAG "${GIT_VERSION}")
    
    # Get everything after the tag
    string(LENGTH "${VERSION_TAG}" TAG_LEN)
    string(SUBSTRING "${GIT_VERSION}" ${TAG_LEN} -1 SUFFIX)
    
    if(SUFFIX AND NOT SUFFIX STREQUAL "")
        set(VERSION_STRING "${VERSION_TAG}${SUFFIX}")
    else()
        set(VERSION_STRING "${VERSION_TAG}")
    endif()
else()
    # No proper semantic version found
    set(VERSION_MAJOR 0)
    set(VERSION_MINOR 0)
    set(VERSION_PATCH 0)
    set(VERSION_STRING "${GIT_VERSION}")
endif()

message(STATUS "Parsed version: ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
message(STATUS "Version string: ${VERSION_STRING}")

# Configure version header from template into the build tree so the
# source tree stays clean and out-of-source builds work.
configure_file(
    ${PROJECT_SOURCE_DIR}/src/core/version.h.in
    ${PROJECT_BINARY_DIR}/src/core/version.h
    @ONLY
)

message(STATUS "Building daedalus version: ${GIT_VERSION}")

# Enable testing globally (must be before add_subdirectory)
enable_testing()

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# C++23 features we rely on: std::expected, std::flat_map, std::span, std::jthread,
# std::stop_token, std::print, deducing this. Requires GCC 13+ / Clang 16+.
#
# When daedalus is the top-level project and no explicit install prefix was
# given, default to a user-local install. Do not override the prefix when we
# are embedded as a dependency (FetchContent / add_subdirectory).
if(DAEDALUS_IS_TOP_LEVEL AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()

# Find pkg-config
find_package(PkgConfig REQUIRED)

# Find other required packages
# §TASK-001 (2026-07-18): nlohmann_json is OPTIONAL via find_package —
# falls back to FetchContent from GitHub if no system package is present.
# AGENTS.md previously claimed the dep is fetched automatically, but
# `find_package(... REQUIRED)` failed out before reaching FetchContent.
# Pattern: quiet find + always-on FetchContent with FIND_PACKAGE_ARGS so
# CMake uses the system package if found and fetches otherwise.
find_package(nlohmann_json QUIET)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED libpng)
# libjpeg (turbo) — JPEG decode/encode for ImageProcessor scaling.
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(POPPLER REQUIRED poppler-cpp)

# OmniNet: C++23 HTTP/WebSocket library, fetched from GitLab on first build.
# On subsequent builds the cached source tree in build/_deps/omninet-src/ is reused.
# Override with -DOMNINET_LOCAL=/path/to/omninet for distro-packaged sources.

include(FetchContent)

# §TASK-001 (2026-07-18): nlohmann_json as a fetchable fallback.
# `find_package(nlohmann_json QUIET)` above may already have populated
# the target via the system package. FIND_PACKAGE_ARGS tells CMake
# "use the existing find_package result if it succeeded; otherwise
# fetch and populate the target". GIT_SHALLOW TRUE keeps the clone
# small (we only need a single tag's tree to consume nlohmann_json.hpp).
#
# Override with -Dnlohmann_json_SOURCE_DIR=/path/to/json for a local
# checkout (e.g. distro-packaged sources or a vendored copy).
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(nlohmann_json)

# §TASK-001 (2026-07-18): when FetchContent_MakeAvailable() actually
# fetches nlohmann_json (i.e. find_package above did NOT find a system
# copy), its INTERFACE target is not registered in any install export
# set. Without help, our `install(EXPORT daedalusTargets)` later errors
# with "target daedalus requires target nlohmann_json that is not in
# any export set". Install the single-include headers from the fetched
# source tree ourselves so consumers of `find_package(daedalus)` can
# still #include <nlohmann/json.hpp>. No-op when nlohmann_json came
# from a system package (the SOURCE_DIR points to build/_deps/, but
# the system package's CMake config already handles its own install).
if(EXISTS "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann/json.hpp")
    install(DIRECTORY "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann"
            DESTINATION "include"
            FILES_MATCHING PATTERN "*.hpp")
endif()

set(OMNINET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(OMNINET_INSTALL OFF CACHE BOOL "" FORCE)

if(DEFINED OMNINET_LOCAL AND EXISTS "${OMNINET_LOCAL}/CMakeLists.txt")
    message(STATUS "[daedalus] Using local OmniNet at ${OMNINET_LOCAL}")
    add_subdirectory("${OMNINET_LOCAL}" omninet-build EXCLUDE_FROM_ALL)
else()
    # §Monorepo (2026-09-24): the declaration lives in cmake/FetchOmniNet.cmake,
    # shared with the superbuild root. In a monorepo build the root already
    # included it (first declaration wins — this include is a no-op and the
    # root's is used); standalone daedalus builds declare it here. Either
    # way exactly ONE declaration/fetch exists per build tree.
    message(STATUS "[daedalus] Fetching OmniNet from gitlab.com/leestripp/omninet")
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchOmniNet.cmake)
    FetchContent_MakeAvailable(OmniNet)
endif()

# Patch OmniNet bugs (infinite recursion in Formatter::kv/bool and
# KeyValueList::add/bool) that the current main branch has.
# These patches apply to the fetched source in build/_deps/omninet-src/.
# Only apply when using the fetched OmniNet (not OMNINET_LOCAL).
#
# §TASK-006 (2026-07-18): patches are idempotent — `string(REPLACE)`
# on a non-matching pattern is a no-op, so once upstream fixes these
# calls the patches silently stop doing anything. Pinned to OmniNet
# commit XXXXXXX (TBD via `git describe --tags --always` in CI) so a
# future clean reconfigure against a fixed upstream won't regress.
# Tracked as https://gitlab.com/leestripp/omninet/-/issues/1 (placeholder)
# — when that issue closes, remove this block.
if(NOT DEFINED OMNINET_LOCAL OR NOT EXISTS "${OMNINET_LOCAL}/CMakeLists.txt")
    # Use the actual populated OmniNet source directory. With FetchContent this
    # may live in the consumer's build tree rather than under PROJECT_BINARY_DIR.
    file(READ "${OmniNet_SOURCE_DIR}/tools/tools_common/colors.cpp" _colors_src)
    string(REPLACE
        "return kv(key, value ? \"true\" : \"false\");"
        "return kv(key, std::string(value ? \"true\" : \"false\"));"
        _colors_src "${_colors_src}")
    file(WRITE "${OmniNet_SOURCE_DIR}/tools/tools_common/colors.cpp" "${_colors_src}")

    file(READ "${OmniNet_SOURCE_DIR}/tools/tools_common/table.cpp" _table_src)
    string(REPLACE
        "return add(key, value ? \"true\" : \"false\");"
        "return add(key, std::string(value ? \"true\" : \"false\"));"
        _table_src "${_table_src}")
    file(WRITE "${OmniNet_SOURCE_DIR}/tools/tools_common/table.cpp" "${_table_src}")
endif()

# §TASK-007 (2026-07-18): OmniNet's own test_http_client and test_sync_http_server
# use overlapping hardcoded port ranges (18080-18088). When ctest runs them in
# parallel they collide. Serialize them with a resource lock so the full suite
# passes reliably.
if(TEST test_http_client AND TEST test_sync_http_server)
    set_tests_properties(test_http_client test_sync_http_server
                         PROPERTIES RESOURCE_LOCK "omninet_http_18080_range")
endif()

# Sanity check: OmniNet header must exist, otherwise the FetchContent_MakeAvailable
# above did not actually provide the target/headers (e.g. repo renamed target).
get_target_property(_omninet_inc OmniNet INCLUDE_DIRECTORIES)
if(NOT _omninet_inc)
    message(FATAL_ERROR "[daedalus] OmniNet fetched but `OmniNet` target has no include directories. "
                        "Check that leestripp/omninet exports an `OmniNet` CMake target.")
endif()

# Unit tests (no external framework - uses simple CTest-style assertions)
# See tests/CMakeLists.txt for test definitions

# Source files organized by module
# ----------------------------------------------------------------------------
# Phase 1 (refactor to static library): the existing modules compile into a
# single static library `daedalus` that the CLI executable links against. The
# CLI stays as our regression harness (it must behave identically until the
# very last refactor phase).
# ----------------------------------------------------------------------------

set(CORE_SOURCES
	src/core/FileIO.cpp
	src/core/SettingsStore.cpp
	src/core/SkillManager.cpp
	src/core/Vault.cpp
	src/core/utils.cpp
	src/core/ImageProcessor.cpp
	src/core/Subprocess.cpp
	src/core/SessionMemory.cpp
	src/core/Logger.cpp
	src/core/BackgroundProcessManager.cpp
	src/core/Messages.cpp
	src/core/SystemPromptBuilder.cpp
	src/core/ThreadPool.cpp
	src/core/Metrics.cpp
	src/core/SerperConfig.cpp
	src/core/SearxngConfig.cpp
)

set(API_SOURCES
	src/api/LLMClient.cpp
	src/api/HttpSession.cpp
	src/api/providers/ProviderRegistry.cpp     # §ProviderPlugins
	src/api/providers/OpenAiCompatiblePlugin.cpp # §ProviderPlugins
	src/api/providers/MiniMaxPlugin.cpp        # §ProviderPlugins Phase 3
	src/api/providers/MoonshotPlugin.cpp       # §ProviderPlugins Phase 4
	src/api/providers/LmStudioPlugin.cpp       # §ProviderPlugins Phase 5
)

set(MCP_SOURCES
	src/mcp/McpClient.cpp
	src/mcp/McpServerManager.cpp
	src/mcp/StdioMcpTransport.cpp
	src/mcp/HttpMcpTransport.cpp
)

set(TOOLS_SOURCES
	src/tools/ToolHandler.cpp
	src/tools/ToolFactory.cpp
	src/tools/ToolFilter.cpp
	src/tools/McpProxiedTool.cpp
	src/tools/McpStatusTool.cpp
	src/tools/VaultTool.cpp
	src/tools/UserTool.cpp
	src/tools/SetStatusTool.cpp
	src/tools/BashTool.cpp
	src/tools/BashChainer.cpp
	src/tools/BashCommandExecutor.cpp
	src/tools/SanitizedOutputStream.cpp
	src/tools/BashPathChecker.cpp
	src/tools/SkillTool.cpp
	src/tools/MemoryTool.cpp
	src/tools/SerperSearchTool.cpp
	src/tools/SearxngSearchTool.cpp
	src/tools/ListToolsTool.cpp
	src/tools/McpControlTool.cpp
	src/tools/ReadImageTool.cpp
	src/tools/PDFTool.cpp
	src/tools/TodoListTool.cpp
	src/tools/TodoListService.cpp
)

# Public library API sources (Phase 1: headers + thin stubs).
set(LIBRARY_PUBLIC_SOURCES
	src/library/Agent.cpp
	src/library/AgentImpl.cpp
	src/library/AgentSink.cpp
	src/library/AgentState.cpp
	src/library/ErrorKind.cpp
	src/library/ThinkingLevel.cpp
	src/library/LogLevel.cpp
	src/library/LibraryToolContext.cpp
	src/library/default_paths.cpp
	src/library/paths.cpp           # Plan.md Phase 1: XDG paths singleton
	src/library/project_paths.cpp   # Plan.md Phase 1: per-project path resolver
	src/library/model_discovery.cpp # Provider-agnostic /v1/models discovery
	src/library/a2a/A2aClient.cpp   # A2A (Agent2Agent Protocol) client
	src/library/a2a/A2aServer.cpp   # A2A (Agent2Agent Protocol) server
)

set(ALL_SOURCES
	${CORE_SOURCES}
	${API_SOURCES}
	${MCP_SOURCES}
	${TOOLS_SOURCES}
	${LIBRARY_PUBLIC_SOURCES}
)

# ----------------------------------------------------------------------------
# daedalus library
# ----------------------------------------------------------------------------
add_library(daedalus STATIC ${ALL_SOURCES})

# Expose the git version to the library so demos / tests can query it via
# daedalus::Agent::library_version(). PUBLIC so it propagates to consumers
# (demos, tests, third-party embeds).
target_compile_definitions(daedalus PUBLIC
    DAEDALUS_GIT_VERSION="${GIT_VERSION}"
)

# Propagate the C++23 requirement to anyone linking against daedalus.
# This is needed because the public headers use std::expected, std::stop_token,
# and other C++23 features.
target_compile_features(daedalus PUBLIC cxx_std_23)

target_include_directories(daedalus
	PUBLIC
		$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
	PRIVATE
		${PROJECT_BINARY_DIR}/src
		${PROJECT_SOURCE_DIR}/src
		${PNG_INCLUDE_DIRS}
		${JPEG_INCLUDE_DIRS}
		${POPPLER_INCLUDE_DIRS}
)

target_link_libraries(daedalus
	PUBLIC
		# nlohmann_json IS exposed via the public API (json types in AgentConfig/LogLevel).
		nlohmann_json::nlohmann_json
		# OpenSSL — TLS support uses libssl/libcrypto.
		OpenSSL::Crypto
		OpenSSL::SSL
		# OmniNet is used internally (LLMClient/NetworkTool use it for
		# HTTP/SSE/WebSocket). Not exposed via public headers, and we
		# don't want to force consumers to find_package(OmniNet).
		# Marked BUILD_INTERFACE only → not included in the installed
		# EXPORT set. Hosts that use NetworkTool/McpServerManager
		# will need to also link OmniNet.
		"$<BUILD_INTERFACE:OmniNet>"
	PRIVATE
		# libpng + libjpeg are used by ImageProcessor in core. Keeping
		# them PRIVATE means they're baked into libdaedalus.a but not
		# declared as link deps to consumers. poppler-cpp is used by
		# PDFTool (§PDF-CORE 2026-08-31: read_pdf is a built-in tool
		# again, so the required poppler dep links here, not in demos).
		${PNG_LIBRARIES}
		${JPEG_LIBRARIES}
		${POPPLER_LIBRARIES}
)

# Visibility hint: when later turned into a shared library, only the
# daedalus::* public API is exported (compile-time check lives in
# include/daedalus/*; everything else is in src/library or src/).
set_target_properties(daedalus PROPERTIES
	CXX_VISIBILITY_PRESET hidden
	POSIX_INHERIT_EPMD TRUE
)

# No executable is defined here on purpose — the library is purely a
# static archive. The CLI lives in demos/daedalus-cli/ and is added
# via add_subdirectory(demos) below. This makes the static-lib build
# the default target, with demos as opt-in executables that exercise
# it.

install(TARGETS daedalus
        EXPORT daedalusTargets
        ARCHIVE DESTINATION "lib"
        INCLUDES DESTINATION "include")

# ----------------------------------------------------------------------------
# find_package(daedalus) support
#
# Generates:
#   <prefix>/lib/cmake/daedalus/daedalusConfig.cmake
#   <prefix>/lib/cmake/daedalus/daedalusConfigVersion.cmake
#   <prefix>/lib/cmake/daedalus/daedalusTargets.cmake
#   <prefix>/lib/cmake/daedalus/daedalusTargets-<buildtype>.cmake
#
# Hosts that embed libdaedalus.a add to their CMakeLists.txt:
#   find_package(daedalus REQUIRED)
#   target_link_libraries(my_host PRIVATE daedalus::daedalus)
# ----------------------------------------------------------------------------
include(CMakePackageConfigHelpers)

# NOTE: libdaedalus.a is a STATIC library. Only targets that are in the
# install export set are propagated to consumers via
# INTERFACE_LINK_LIBRARIES. OmniNet is kept BUILD_INTERFACE-only, so
# consumers using find_package(daedalus) need to provide OmniNet
# themselves if they use features that require it.
set(DAEDALUS_LINK_LIBS "")

# Generate the version file. The project extracts VERSION_MAJOR/MINOR/PATCH
# from the git tag manually rather than via project(... VERSION x.y.z),
# so PROJECT_VERSION is empty. Build the version string here.
set(_DAEDALUS_CFG_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")

# Target name of the static library on each platform.
if(WIN32)
    set(DAEDALUS_LIB_NAME "daedalus.lib")
else()
    set(DAEDALUS_LIB_NAME "libdaedalus.a")
endif()

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/daedalusConfigVersion.cmake"
    VERSION "${_DAEDALUS_CFG_VERSION}"
    COMPATIBILITY SameMajorVersion
)

# Generate the main config file. We use configure_file (not
# configure_package_config_file) so we can pass the lib name as an
# extra substitution. The @PACKAGE_*@ variables below are normally
# auto-substituted by configure_package_config_file; we set them
# ourselves here so they work with configure_file too.
set(PACKAGE                "${CMAKE_INSTALL_PREFIX}")
set(PACKAGE_VERSION        "${_DAEDALUS_CFG_VERSION}")
set(PACKAGE_VERSION_MAJOR  "${VERSION_MAJOR}")
set(PACKAGE_VERSION_MINOR  "${VERSION_MINOR}")
set(PACKAGE_VERSION_PATCH  "${VERSION_PATCH}")
set(PACKAGE_VERSION_TWEAK  "0")
set(PACKAGE_VERSION_COUNT  "3")

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/DaedalusConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/daedalusConfig.cmake"
    @ONLY
)

install(EXPORT daedalusTargets
        FILE "daedalusTargets.cmake"
        NAMESPACE daedalus::
        DESTINATION "lib/cmake/daedalus")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/daedalusConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/daedalusConfigVersion.cmake"
        DESTINATION "lib/cmake/daedalus")

# Install the public headers so downstream `find_package(daedalus)` works.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/daedalus"
        DESTINATION "include"
        FILES_MATCHING PATTERN "*.h")
install(FILES "${PROJECT_SOURCE_DIR}/include/daedalus.h"
        DESTINATION "include")

# Install the example settings template to ~/.daedalus/settings/ (only if no
# settings.json already exists). This preserves any user modifications.
# The install is from the canonical example at examples/settings.json.example;
# the canonical JSON key for the Serper.dev search tool is "serper-search".
# Only do this for a stand-alone daedalus install; embedding the library via
# FetchContent / add_subdirectory() should not touch the user's home directory.
if(DAEDALUS_IS_TOP_LEVEL)
    set(USER_SETTINGS_DIR "$ENV{HOME}/.daedalus/settings")
    set(EXAMPLE_SETTINGS_FILE "${PROJECT_SOURCE_DIR}/examples/settings.json.example")
    if(NOT EXISTS "${USER_SETTINGS_DIR}/settings.json")
        if(EXISTS "${EXAMPLE_SETTINGS_FILE}")
            install(FILES "${EXAMPLE_SETTINGS_FILE}"
                    DESTINATION "${USER_SETTINGS_DIR}"
                    RENAME "settings.json")
        else()
            message(WARNING "Example settings file not found at ${EXAMPLE_SETTINGS_FILE}; skipping settings install.")
        endif()
    else()
        message(STATUS "User settings already exist at ${USER_SETTINGS_DIR}, skipping installation")
    endif()
endif()

# Demos and tests default to ON for a stand-alone build, OFF when daedalus
# is embedded as a dependency (FetchContent / add_subdirectory).
option(DAEDALUS_BUILD_DEMOS "Build daedalus demo executables" ${DAEDALUS_IS_TOP_LEVEL})
option(DAEDALUS_BUILD_TESTS "Build daedalus unit tests" ${DAEDALUS_IS_TOP_LEVEL})

# Include demos if the option is on (default: ON).
# Demos are standalone executables that link against libdaedalus.a and
# exercise the library from outside its own sources.
# Placed BEFORE tests so example tool libraries (e.g. daedalus_example_tools)
# are defined when test targets that need them are configured.
if(DAEDALUS_BUILD_DEMOS)
    add_subdirectory(demos)
endif()

# Include unit tests if tests directory exists
if(DAEDALUS_BUILD_TESTS AND EXISTS "${PROJECT_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()

# ============================================================================
# cppcheck static analysis
#
# OmniNet (fetched above) already adds its own `cppcheck` target when cppcheck
# is on PATH. To avoid a target-name collision, we prefix ours with `daedalus_`
# so users get both targets side-by-side.
#
# Note: clang-tidy used to live here. It was removed because gcc/g++ is the
# default compiler again (see the "Compiler selection" block at the top of
# this file) and clang-tidy is clang-only. cppcheck's heuristic parser works
# against gcc-compiled TUs without needing compile_commands.json, so it's
# the right choice for this toolchain.
# ============================================================================
find_program(CPPCHECK cppcheck)
if(CPPCHECK)
    message(STATUS "cppcheck found: ${CPPCHECK}")

    # cppcheck settings
    set(CPPCHECK_FLAGS
        "--enable=all"
        "--std=c++23"
        "--platform=unix64"
        "--quiet"
        "--inline-suppr"
        "--suppress=missingIncludeSystem"
        "--template=message"
        "-i${PROJECT_BINARY_DIR}/_deps"
    )

    # Sources that belong to libdaedalus.a. Explicit list prevents cppcheck
    # from wandering into fetched dependencies (build/_deps/) or demo/test code.
    set(CPPCHECK_SOURCES
        ${CORE_SOURCES}
        ${API_SOURCES}
        ${MCP_SOURCES}
        ${TOOLS_SOURCES}
        ${LIBRARY_PUBLIC_SOURCES}
    )

    # Add daedalus-prefixed cppcheck target (OmniNet may have added `cppcheck`)
    add_custom_target(daedalus_cppcheck
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/cppcheck
        COMMAND ${CPPCHECK} ${CPPCHECK_FLAGS}
            --output-file=${PROJECT_BINARY_DIR}/cppcheck_results.txt
            --cppcheck-build-dir=${PROJECT_BINARY_DIR}/cppcheck
            ${CPPCHECK_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Running cppcheck static analysis on daedalus library sources..."
        VERBATIM
    )

    # Also add a quick cppcheck target that only checks src/ (no tests, no demos)
    add_custom_target(daedalus_cppcheck_quick
        COMMAND ${CPPCHECK} ${CPPCHECK_FLAGS}
            ${CPPCHECK_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Running cppcheck on daedalus library sources (quick mode)..."
        VERBATIM
    )

    # Add cppcheck.xml target for CI/CD integration
    add_custom_target(daedalus_cppcheck_xml
        COMMAND ${CPPCHECK} ${CPPCHECK_FLAGS}
            --xml --output-file=${PROJECT_BINARY_DIR}/cppcheck.xml
            ${CPPCHECK_SOURCES}
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        COMMENT "Running cppcheck on daedalus library sources with XML output..."
        VERBATIM
    )

    message(STATUS "cppcheck targets available:")
    message(STATUS "  make daedalus_cppcheck        - Full analysis (daedalus library sources)")
    message(STATUS "  make daedalus_cppcheck_quick  - Quick check (daedalus library sources)")
    message(STATUS "  make daedalus_cppcheck_xml    - XML output for CI/CD")
else()
    message(STATUS "cppcheck not found - static analysis skipped")
    message(STATUS "Install with: apt install cppcheck (Debian/Ubuntu)")
    message(STATUS "                brew install cppcheck (macOS)")
endif()
