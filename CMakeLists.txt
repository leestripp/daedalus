cmake_minimum_required(VERSION 3.28)

# ============================================================================
# §Phase 9.9: prefer clang/clang++ when no compiler was forced by the user.
# The user can still force gcc with `CC=gcc CXX=g++ cmake -S . -B build`.
# clang gives much better diagnostics and is required for clang-tidy to be
# happy.  Must run BEFORE project() so CMake doesn't latch onto the
# system default gcc.
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(_DAEDALUS_CLANGXX clang++)
    if(_DAEDALUS_CLANGXX)
        set(CMAKE_CXX_COMPILER "${_DAEDALUS_CLANGXX}" CACHE FILEPATH "C++ compiler")
        message(STATUS "[daedalus] defaulting CXX to ${_DAEDALUS_CLANGXX} (set CXX= to override)")
    endif()
    unset(_DAEDALUS_CLANGXX)
endif()
if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(_DAEDALUS_CLANG clang)
    if(_DAEDALUS_CLANG)
        set(CMAKE_C_COMPILER "${_DAEDALUS_CLANG}" CACHE FILEPATH "C compiler")
        message(STATUS "[daedalus] defaulting CC to ${_DAEDALUS_CLANG} (set CC= to override)")
    endif()
    unset(_DAEDALUS_CLANG)
endif()

project(daedalus LANGUAGES CXX C)

# §Phase 9.9: emit compile_commands.json so clang-tidy (and any IDE)
# can map source files to their exact compiler invocation.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Automatic version from git tags
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
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

# Configure version header from template
configure_file(
    ${CMAKE_SOURCE_DIR}/src/core/version.h.in
    ${CMAKE_SOURCE_DIR}/src/core/version.h
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
# Install to users home dir.
set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local")

# Find pkg-config
find_package(PkgConfig REQUIRED)

# Find other required packages
find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(POPPLER REQUIRED poppler-cpp)

# OmniNet: C++23 HTTP/WebSocket library, fetched from GitLab on first build.
# On subsequent builds the cached source tree in build/_deps/omninet-src/ is reused.
# Override with -DOMNINET_LOCAL=/path/to/omninet for distro-packaged sources.

include(FetchContent)

# Pre-declare nlohmann_json BEFORE OmniNet's CMakeLists.txt runs.
# OmniNet FetchContents its own copy, but we already have the system
# nlohmann_json. By declaring it first with FIND_PACKAGE_ARGS, CMake's
# FetchContent_MakeAvailable inside OmniNet's script will use our existing
# target instead of trying to create a second one with the same name.
FetchContent_Declare(
    nlohmann_json
    FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(nlohmann_json)

set(OMNINET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(OMNINET_INSTALL OFF CACHE BOOL "" FORCE)

if(DEFINED OMNINET_LOCAL AND EXISTS "${OMNINET_LOCAL}/CMakeLists.txt")
    message(STATUS "[daedalus] Using local OmniNet at ${OMNINET_LOCAL}")
    add_subdirectory("${OMNINET_LOCAL}" omninet-build EXCLUDE_FROM_ALL)
else()
    message(STATUS "[daedalus] Fetching OmniNet from gitlab.com/leestripp/omninet")
    FetchContent_Declare(
        OmniNet
        GIT_REPOSITORY https://gitlab.com/leestripp/omninet.git
        GIT_TAG        main
        # NOT shallow: OmniNet's CMakeLists detects its version via
        # `git describe --tags --abbrev=0`. A shallow clone only carries
        # the single branch tip and has no tag history, so the lookup
        # fails and falls back to FALLBACK_VERSION ("0.1.0"). A full
        # clone keeps all tags, so e.g. HEAD just past v0.8.2 reports
        # `0.8.2` correctly instead of the misleading `0.1.0`.
    )
    FetchContent_MakeAvailable(OmniNet)
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
	src/core/SettingsMigrator.cpp
	src/core/SkillManager.cpp
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
)

set(API_SOURCES
	src/api/LLMClient.cpp
	src/api/CurlMultiSession.cpp
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
	src/tools/FileTool.cpp
	src/tools/McpProxiedTool.cpp
    src/core/SerperConfig.cpp
    src/tools/SearchTool.cpp
	src/tools/UserTool.cpp
	src/tools/SetStatusTool.cpp
	src/tools/GitTool.cpp
	src/tools/BashTool.cpp
	src/tools/BashChainer.cpp
	src/tools/BashCommandExecutor.cpp
	src/tools/BashPathChecker.cpp
	src/tools/PDFTool.cpp
	src/tools/SkillTool.cpp
	src/tools/MemoryTool.cpp
	src/tools/SystemInfoTool.cpp
	src/tools/BackgroundTool.cpp
	src/tools/NetworkTool.cpp
)

# Public library API sources (Phase 1: headers + thin stubs).
set(LIBRARY_PUBLIC_SOURCES
	src/library/Agent.cpp
	src/library/AgentImpl.cpp
	src/library/AgentSink.cpp
	src/library/AgentState.cpp
	src/library/LogLevel.cpp
	src/library/LibraryToolContext.cpp
	src/library/default_paths.cpp
	src/library/paths.cpp           # Plan.md Phase 1: XDG paths singleton
	src/library/project_paths.cpp   # Plan.md Phase 1: per-project path resolver
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

target_include_directories(daedalus
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
		${POPPLER_INCLUDE_DIRS}
	PRIVATE
		${CMAKE_SOURCE_DIR}/src
		${PNG_INCLUDE_DIRS}
)

target_link_libraries(daedalus
	PUBLIC
		# nlohmann_json IS exposed via the public API (json types in AgentConfig/LogLevel).
		nlohmann_json::nlohmann_json
		# CURL is used internally (HTTP networking via libcurl).
		# It is NOT exposed via public headers — propagates to consumers
		# as an INTERFACE_LINK_LIB so they get the runtime symbols.
		CURL::libcurl
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
		# libpng + poppler-cpp are only used by ImageProcessor and
		# PDFTool. Keeping them PRIVATE means they're baked into
		# libdaedalus.a but not declared as link deps to consumers.
		# Hosts that don't use those tools don't need to link them.
		# Hosts that do (and care about not linking the whole world)
		# can rebuild daedalus without those tools.
		${PNG_LIBRARIES}
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

# NOTE: libdaedalus.a is a STATIC library — all symbols (including
# OmniNet, CURL, nlohmann_json, etc.) are baked in at build time.
# Consumers that link against `daedalus::daedalus` therefore do NOT
# need to link any of these themselves. We deliberately leave
# INTERFACE_LINK_LIBRARIES empty to avoid spurious link-time
# complaints about "target X not in any export set".
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
    "${CMAKE_SOURCE_DIR}/cmake/DaedalusConfig.cmake.in"
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
install(DIRECTORY "${CMAKE_SOURCE_DIR}/include/daedalus"
        DESTINATION "include"
        FILES_MATCHING PATTERN "*.h")
install(FILES "${CMAKE_SOURCE_DIR}/include/daedalus.h"
        DESTINATION "include")

# Install the example settings template to ~/.daedalus/settings/ (only if no
# settings.json already exists). This preserves any user modifications.
# The install is from the canonical example at examples/settings.json.example;
# the canonical JSON key for the Serper.dev search tool is "serper-search".
set(USER_SETTINGS_DIR "$ENV{HOME}/.daedalus/settings")
set(EXAMPLE_SETTINGS_FILE "${CMAKE_SOURCE_DIR}/examples/settings.json.example")
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

# Include unit tests if tests directory exists
if(EXISTS "${CMAKE_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()

# Include demos if the option is on (default: ON).
# Demos are standalone executables that link against libdaedalus.a and
# exercise the library from outside its own sources.
add_subdirectory(demos)

# Static analysis is provided by clang-tidy via the daedalus_clang_tidy_*
# targets further down in this file. cppcheck used to live here; it was
# removed in 2026-07 because the project standardised on clang-tidy
# (which consumes compile_commands.json and so reuses the exact flags
# the TUs were built with, unlike cppcheck's heuristic parser).

# ============================================================================
# §Phase 9.9: clang-tidy static analysis.
# ============================================================================
# Reads compile_commands.json (enabled above via CMAKE_EXPORT_COMPILE_COMMANDS=ON)
# so each .cpp / .h is analyzed with its exact compile flags.
# If clang-tidy is missing, the targets silently no-op and a message is printed.

find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
if(CLANG_TIDY_EXECUTABLE)
    message(STATUS "clang-tidy found: ${CLANG_TIDY_EXECUTABLE}")

    # Restrict analysis to first-party sources — skip curl, poppler,
    # OmniNet, sqlite, openssl etc. that come in via build/_deps/ or
    # system headers.  Matches the .clang-tidy HeaderFilterRegex.
    set(HEADER_FILTER_REGEX "(^|/)(src|include|tests|demos|tools)/.*\\.h(pp)?$")

    # Helper macro: adds a clang-tidy target that runs over a directory.
    # clang-tidy reads compile_commands.json which CMake auto-generates
    # when CMAKE_EXPORT_COMPILE_COMMANDS=ON.  We use file(GLOB_RECURSE)
    # at configure time so the actual file list is passed — passing the
    # raw "${DIR}/*.cpp" glob would not work because CMake does not
    # invoke a shell to expand globs in custom_target commands.
    #
    # VERBATIM is required: without it the generated Makefile recipe
    # passes arguments unescaped to /bin/sh, and the trailing `$` in
    # HEADER_FILTER_REGEX combined with shell quoting (the previous
    # implementation wrapped it in single quotes) produced `$'` —
    # which dash interprets as the start of an ANSI-C quoted string
    # and fails with "Unterminated quoted string". VERBATIM lets
    # CMake shell-escape each argument correctly, so we can pass
    # the regex as a plain bare argument instead of quoting it.
    macro(add_clang_tidy_target TARGET_NAME DIR)
        file(GLOB_RECURSE _CT_SOURCES CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/${DIR}/*.cpp"
            "${CMAKE_SOURCE_DIR}/${DIR}/*.h")
        add_custom_target(${TARGET_NAME}
            COMMAND ${CLANG_TIDY_EXECUTABLE}
                -p ${CMAKE_BINARY_DIR}
                --quiet
                -header-filter=${HEADER_FILTER_REGEX}
                ${_CT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "Running clang-tidy for ${TARGET_NAME}"
            VERBATIM
        )
    endmacro()

    # Per-tree targets.  Prefixed `daedalus_` because OmniNet already
    # registers its own `clang_tidy*` / `static_analysis` targets.
    add_clang_tidy_target(daedalus_clang_tidy_lib   src)
    add_clang_tidy_target(daedalus_clang_tidy_tests tests)
    add_clang_tidy_target(daedalus_clang_tidy_demos demos)
    add_clang_tidy_target(daedalus_clang_tidy_tools tools)

    # Master target: run all four.
    add_custom_target(daedalus_clang_tidy
        COMMAND ${CMAKE_COMMAND} -E echo "Running all daedalus clang-tidy analyses..."
        COMMAND ${CMAKE_COMMAND} --build . --target daedalus_clang_tidy_lib
        COMMAND ${CMAKE_COMMAND} --build . --target daedalus_clang_tidy_tests
        COMMAND ${CMAKE_COMMAND} --build . --target daedalus_clang_tidy_demos
        COMMAND ${CMAKE_COMMAND} --build . --target daedalus_clang_tidy_tools
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running all daedalus clang-tidy analyses"
    )

    # Alias for `make daedalus_static_analysis`.
    add_custom_target(daedalus_static_analysis
        COMMAND ${CMAKE_COMMAND} --build . --target daedalus_clang_tidy
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running daedalus_static_analysis (alias for daedalus_clang_tidy)"
    )

    message(STATUS "daedalus clang-tidy targets created: daedalus_clang_tidy, daedalus_clang_tidy_{lib,tests,demos,tools}")
else()
    message(STATUS "clang-tidy not found - static analysis disabled (install clang-tidy to enable)")
endif()
