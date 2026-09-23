cmake_minimum_required(VERSION 3.28)

# ============================================================================
# icarus - CLI AI coding agent built on the daedalus library
#
# §Monorepo (2026-09-24): dual-mode daedalus consumption.
#   - In-tree (monorepo superbuild): the `daedalus` target already exists
#     (root added daedalus/ first) — link it directly.
#   - Standalone (cmake -S icarus): FetchContent pulls the daedalus monorepo
#     from GitLab main; its superbuild root builds only the library when it
#     is not the top-level project, so no GTK/host deps leak in.
# The old FetchContent_Populate + ExternalProject_Add double-build is gone:
# both modes link the `daedalus` CMake target, which propagates OmniNet,
# OpenSSL, nlohmann_json and the static lib's PRIVATE deps (png/jpeg/poppler)
# transitively.
# ============================================================================

# Prefer gcc/g++ when no compiler was forced by the user (standalone builds;
# in the superbuild CMAKE_CXX_COMPILER is already set and this is a no-op)
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(_ICARUS_GXX g++)
    if(_ICARUS_GXX)
        set(CMAKE_CXX_COMPILER "${_ICARUS_GXX}" CACHE FILEPATH "C++ compiler")
        message(STATUS "[icarus] defaulting CXX to ${_ICARUS_GXX} (set CXX= to override)")
    endif()
    unset(_ICARUS_GXX)
endif()
if(NOT DEFINED CMAKE_C_COMPILER)
    find_program(_ICARUS_GCC gcc)
    if(_ICARUS_GCC)
        set(CMAKE_C_COMPILER "${_ICARUS_GCC}" CACHE FILEPATH "C compiler")
        message(STATUS "[icarus] defaulting CC to ${_ICARUS_GCC} (set CC= to override)")
    endif()
    unset(_ICARUS_GCC)
endif()

project(icarus LANGUAGES CXX C)

# Emit compile_commands.json for tooling (IDEs, static analyzers, etc.)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++23 required for daedalus
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# Default install prefix: the current user's ~/.local. Only applied when
# the caller didn't pass -DCMAKE_INSTALL_PREFIX=... and HOME is usable
# (skipped when HOME is unset or "/", e.g. odd container build steps).
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND
   DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "" AND
   NOT "$ENV{HOME}" STREQUAL "/")
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local"
        CACHE PATH "Install prefix" FORCE)
endif()

# Enable CTest so add_test() calls in subdirectories are registered.
enable_testing()

# Find required packages
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
# §Task A3 (CodeMax.md §16): libpng is required because libdaedalus.a
# has unresolved references to png_* symbols.
pkg_check_modules(PNG REQUIRED libpng)
# §PDF-CORE (2026-08-31): daedalus 1.0.2 moved ReadPDFTool back into the
# library (src/tools/PDFTool.cpp), so libdaedalus.a now carries poppler_*
# references. Static libs don't propagate their PRIVATE link deps, so
# poppler-cpp must be checked AND linked here — same pattern as libpng.
pkg_check_modules(POPPLER REQUIRED poppler-cpp)
pkg_check_modules(UUID REQUIRED uuid)
# §JPEG (2026-08-11): daedalus 0.10.x ImageProcessor (read_image JPEG
# scaling, §JPEG-SCALE) leaves unresolved jpeg_* symbols in libdaedalus.a,
# so libjpeg must be linked here — same pattern as libpng above.
pkg_check_modules(JPEG REQUIRED libjpeg)

# ============================================================================
# daedalus (dual-mode, see header comment)
# ============================================================================
include(FetchContent)
if(TARGET daedalus)
    # In-tree monorepo build.
    set(DAEDALUS_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../daedalus)
else()
    # Standalone: fetch the monorepo, build only the library.
    set(DAEDALUS_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(DAEDALUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        daedalus
        GIT_REPOSITORY https://gitlab.com/leestripp/daedalus.git
        GIT_TAG main
        # NOT shallow: daedalus derives its version via git describe.
    )
    FetchContent_MakeAvailable(daedalus)
    set(DAEDALUS_SRC_DIR ${daedalus_SOURCE_DIR}/daedalus)
endif()

# Include dirs for headers that public daedalus headers pull in transitively
# (<daedalus/settings.h> → OmniNet's <db/Database.h>, nlohmann/json.hpp).
# Linking the `daedalus` target already propagates these; the explicit list
# is for targets that need the headers WITHOUT linking the library (the
# header-only icarus_test_util). Computed for both build modes — each entry
# is added only when it actually exists (e.g. nlohmann_json may come from a
# system package, sqlite_vec is fetched by OmniNet).
set(DAEDALUS_DEP_INCLUDES "")
FetchContent_GetProperties(OmniNet)
FetchContent_GetProperties(nlohmann_json)
FetchContent_GetProperties(sqlite_vec)
foreach(_dep_inc
        "${omninet_SOURCE_DIR}/src"
        "${nlohmann_json_SOURCE_DIR}/include"
        "${nlohmann_json_SOURCE_DIR}/single_include"
        "${sqlite_vec_SOURCE_DIR}")
    if(_dep_inc AND EXISTS "${_dep_inc}")
        list(APPEND DAEDALUS_DEP_INCLUDES "${_dep_inc}")
    endif()
endforeach()

# §Task 1 (CodeMax.md §16.3): linenoise — single-file guerrilla line
# editing library. Pinned to commit a473823 (2026-05-02). BSD-2-Clause,
# see copyright header in linenoise.c. Built as a static C library and
# linked into icarus.
# CMP0169 OLD silences the FetchContent_Populate deprecation warning (we
# populate linenoise manually because it has no CMakeLists of its own).
cmake_policy(SET CMP0169 OLD)
FetchContent_Declare(
    linenoise
    GIT_REPOSITORY https://github.com/antirez/linenoise.git
    GIT_TAG        a473823d74b93eab2ba83480df16ed37617493f2
)
FetchContent_Populate(linenoise)

add_library(linenoise STATIC "${linenoise_SOURCE_DIR}/linenoise.c")
target_include_directories(linenoise PUBLIC "${linenoise_SOURCE_DIR}")
set_target_properties(linenoise PROPERTIES C_STANDARD 99)

# §Settings TUI (2026-07-30): FTXUI, pinned to the same v7.0.1 tag the
# daedalus settings-demo uses (see daedalus demos/daedalus-settings-tui/
# CMakeLists.txt). The demo's tab sources are compiled straight into the
# icarus binary (below) so bare /settings can open the settings TUI
# in-process instead of shelling out to a separate executable. In the
# monorepo superbuild the demo may already have populated ftxui — the first
# FetchContent declaration wins, and both pin v7.0.1.
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v7.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ftxui)

# ============================================================================
# icarus executable
# ============================================================================
add_executable(icarus
    src/main.cpp
    src/ConsoleAgentSink.cpp
    src/ui/AnsiRenderer.cpp
    src/ui/StatusHud.cpp
    src/migrate.cpp
    src/a2a_tools.cpp
    src/a2a_agent.cpp
    # §Settings TUI (2026-07-30): the daedalus settings-demo tab sources,
    # compiled in-process so /settings can open the TUI. Its main.cpp is
    # intentionally excluded (icarus has its own entry point).
    ${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui/SettingsApp.cpp
    ${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui/ProvidersTab.cpp
    ${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui/ModelsTab.cpp
    ${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui/McpTab.cpp
    ${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui/AgentTab.cpp
)

# Link against all required libraries. The `daedalus` target transitively
# provides OmniNet, OpenSSL and nlohmann_json (PUBLIC) plus the static
# lib's PRIVATE deps as link-only items; the explicit system libs below are
# kept for clarity and for icarus's own direct uses (uuid, sqlite3, z).
target_link_libraries(icarus
    PRIVATE
        daedalus
        linenoise
        ftxui::screen
        ftxui::dom
        ftxui::component
        OpenSSL::Crypto
        OpenSSL::SSL
        nlohmann_json::nlohmann_json
        ${PNG_LIBRARIES}
        ${JPEG_LIBRARIES}
        ${POPPLER_LIBRARIES}
        sqlite3
        pthread
        dl
        z
        ${UUID_LIBRARIES}
)

target_include_directories(icarus
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${DAEDALUS_SRC_DIR}/include
        ${DAEDALUS_SRC_DIR}/demos/common
        ${DAEDALUS_SRC_DIR}/demos/daedalus-settings-tui
        ${DAEDALUS_DEP_INCLUDES}
        ${PNG_INCLUDE_DIRS}
        ${UUID_INCLUDE_DIRS}
)

# ============================================================================
# Unit tests (§Tests 2026-08-20)
# ============================================================================
# doctest-based unit tests for the CLI host: parse_cli_args,
# resolve_system_prompt, and the --migrate-data module. Registered with
# CTest — run with:  ctest --test-dir build --output-on-failure
option(ICARUS_BUILD_TESTS "Build icarus unit tests" ON)
if(ICARUS_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# §UA 2026-07-29: derive the icarus version from the latest git tag so the
# binary can identify itself in the HTTP User-Agent (AgentConfig::user_agent)
# without a manually maintained version file.
# §Monorepo (2026-09-24): icarus tags are prefixed (`icarus/x.y.z`) so they
# can't collide with the library's bare version tags; strip the prefix.
# Falls back to "dev" when no icarus/* tag exists or git is unavailable.
find_package(Git QUIET)
set(ICARUS_VERSION "dev")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "icarus/*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ICARUS_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(ICARUS_GIT_TAG)
        string(REGEX REPLACE "^icarus/" "" ICARUS_VERSION "${ICARUS_GIT_TAG}")
    endif()
endif()
target_compile_definitions(icarus PRIVATE ICARUS_VERSION="${ICARUS_VERSION}")
message(STATUS "[icarus] version: ${ICARUS_VERSION}")

# Install icarus to <prefix>/bin — ~/.local by default, /usr/local when
# -DCMAKE_INSTALL_PREFIX=/usr/local is passed (e.g. container builds).
install(TARGETS icarus
    RUNTIME DESTINATION "bin"
)

# ============================================================================
# cppcheck static analysis
# ============================================================================
find_program(CPPCHECK_EXECUTABLE cppcheck)
if(CPPCHECK_EXECUTABLE)
    message(STATUS "[icarus] cppcheck found: ${CPPCHECK_EXECUTABLE}")

    file(GLOB_RECURSE _ICARUS_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

    add_custom_target(icarus_cppcheck
        COMMAND ${CPPCHECK_EXECUTABLE}
            --error-exitcode=1
            --enable=warning,style,performance,portability
            --std=c++23
            --language=c++
            --suppress=missingIncludeSystem
            --suppress=unusedStructMember
            --suppress=noExplicitConstructor:*daedalus*
            --suppress=returnByReference:*daedalus*
            -I ${CMAKE_CURRENT_SOURCE_DIR}/src
            -I ${DAEDALUS_SRC_DIR}/include
            -I ${DAEDALUS_SRC_DIR}/demos/common
            ${_ICARUS_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "[icarus] Running cppcheck"
        VERBATIM
    )
else()
    message(STATUS "[icarus] cppcheck not found - static analysis disabled")
endif()
