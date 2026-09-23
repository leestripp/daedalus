cmake_minimum_required(VERSION 3.28)

# ============================================================================
# icarus - CLI AI coding agent built on daedalus library
# Uses FetchContent to get the latest daedalus from the main branch
# ============================================================================

# Prefer gcc/g++ when no compiler was forced by the user
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
# Fetch daedalus source
# ============================================================================
# §Task A4 (CodeMax.md §16): CMP0169 set to OLD to silence the
# FetchContent_Populate deprecation warning. We can't use
# FetchContent_MakeAvailable here because daedalus is built via
# ExternalProject_Add (below), not add_subdirectory.
cmake_policy(SET CMP0169 OLD)

include(FetchContent)
FetchContent_Declare(
    daedalus
    GIT_REPOSITORY https://gitlab.com/leestripp/daedalus.git
    GIT_TAG main
)
FetchContent_Populate(daedalus)
set(DAEDALUS_SRC_DIR ${CMAKE_BINARY_DIR}/_deps/daedalus-src)

# §Task 1 (CodeMax.md §16.3): linenoise — single-file guerrilla line
# editing library. Pinned to commit a473823 (2026-05-02). BSD-2-Clause,
# see copyright header in linenoise.c. Built as a static C library and
# linked into icarus.
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
# in-process instead of shelling out to a separate executable.
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
# Build daedalus as external project
# ============================================================================
set(DAEDALUS_BUILD_DIR ${CMAKE_BINARY_DIR}/daedalus)

include(ExternalProject)
ExternalProject_Add(daedalus_build
    SOURCE_DIR ${DAEDALUS_SRC_DIR}
    BINARY_DIR ${DAEDALUS_BUILD_DIR}
    INSTALL_COMMAND ""
    CMAKE_ARGS -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER} -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DDAEDALUS_DEMO_SETTINGS_TUI=OFF
    BUILD_BYPRODUCTS ${CMAKE_BINARY_DIR}/daedalus/libdaedalus.a
)

# ============================================================================
# OmniNet paths (available after FetchContent_Populate)
# ============================================================================
set(OMNINET_BUILD_DIR ${DAEDALUS_BUILD_DIR}/_deps/omninet-build)
set(FMT_DIR ${OMNINET_BUILD_DIR}/_deps/fmt-src)

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

# Link against all required libraries
target_link_libraries(icarus
    PRIVATE
        ${CMAKE_BINARY_DIR}/daedalus/libdaedalus.a
        ${CMAKE_BINARY_DIR}/daedalus/_deps/omninet-build/libOmniNet.a
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
        # §Settings TUI (2026-07-30): <daedalus/settings.h> includes
        # <db/Database.h>, which ships with the OmniNet sources inside the
        # daedalus ExternalProject build tree.
        ${DAEDALUS_BUILD_DIR}/_deps/omninet-src/src
        ${OMNINET_BUILD_DIR}/_deps/fmt-src/include
        ${OMNINET_BUILD_DIR}/_deps/nlohmann_json-src/include
        ${OMNINET_BUILD_DIR}/_deps/sqlite_vec-src
        ${DAEDALUS_BUILD_DIR}/_deps/sqlite-vec-src
        ${PNG_INCLUDE_DIRS}
        ${UUID_INCLUDE_DIRS}
)

# Make icarus depend on daedalus build.
add_dependencies(icarus daedalus_build)

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
# without a manually maintained version file. Falls back to "dev" when the
# tree has no tags or git is unavailable.
find_package(Git QUIET)
set(ICARUS_VERSION "dev")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE ICARUS_GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(ICARUS_GIT_TAG)
        set(ICARUS_VERSION ${ICARUS_GIT_TAG})
    endif()
endif()
target_compile_definitions(icarus PRIVATE ICARUS_VERSION="${ICARUS_VERSION}")

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
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.h")

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
            -I ${CMAKE_SOURCE_DIR}/src
            -I ${DAEDALUS_SRC_DIR}/include
            -I ${DAEDALUS_SRC_DIR}/demos/common
            ${_ICARUS_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "[icarus] Running cppcheck"
        VERBATIM
    )
else()
    message(STATUS "[icarus] cppcheck not found - static analysis disabled")
endif()
