cmake_minimum_required(VERSION 3.28)

# ============================================================================
# daedalus monorepo — superbuild root
#
#   daedalus/        the agent library (libdaedalus.a) + demos + tests
#   icarus/          CLI host (REPL + A2A server)
#   monkeybyte/      Electron host + standalone C++ loopback backend
#
# Top-level build:
#   cmake -S . -B build && cmake --build build -j$(nproc)
#   ctest --test-dir build --output-on-failure        (or -L lib|cli|app)
#   cmake --build build --target static_analysis
#
# When consumed via FetchContent / add_subdirectory (e.g. a host fetching
# this repo for the library only), both hosts default OFF so no GTK or CLI
# dependencies are required. Override with -DDAEDALUS_BUILD_ICARUS=ON /
# -DDAEDALUS_BUILD_ELECTRON_BACKEND=ON.
# ============================================================================

project(daedalus-suite LANGUAGES CXX C)

# Top-level monorepo build, or embedded via FetchContent/add_subdirectory?
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(DAEDALUS_ROOT_TOP_LEVEL ON)
else()
    set(DAEDALUS_ROOT_TOP_LEVEL OFF)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- component options ------------------------------------------------------
option(DAEDALUS_BUILD_ICARUS "Build the icarus CLI host" ${DAEDALUS_ROOT_TOP_LEVEL})

option(DAEDALUS_BUILD_ELECTRON_BACKEND "Build the standalone Electron backend" ${DAEDALUS_ROOT_TOP_LEVEL})

# One ctest entry point for every suite (labels below give per-component
# slices: lib / cli / app).
enable_testing()

# --- shared dependencies -----------------------------------------------------
# §Monorepo (2026-09-24): declare OmniNet at the top level so the whole tree
# shares ONE FetchContent declaration (and therefore one clone under
# build/_deps). daedalus/cmake/FetchOmniNet.cmake is the single source of
# truth — daedalus/CMakeLists.txt includes the same file for standalone
# library builds; the second include is a no-op (first declaration wins).
# Declaring here costs nothing when OmniNet is never populated (declaration
# alone does no network I/O).
include(FetchContent)
include(${CMAKE_CURRENT_SOURCE_DIR}/daedalus/cmake/FetchOmniNet.cmake)

# --- library (always built) --------------------------------------------------
# DAEDALUS_SUPERBUILD tells daedalus/CMakeLists.txt to treat the monorepo
# root like a standalone library build: demos + tests default ON, user-local
# install prefix, settings-template install. Unset when this repo is embedded
# (e.g. a host FetchContenting the library) so none of that leaks out.
if(DAEDALUS_ROOT_TOP_LEVEL)
    set(DAEDALUS_SUPERBUILD ON)
endif()
add_subdirectory(daedalus)

# --- hosts -------------------------------------------------------------------
if(DAEDALUS_BUILD_ICARUS)
    add_subdirectory(icarus)
endif()
if(DAEDALUS_BUILD_ELECTRON_BACKEND)
    add_subdirectory(monkeybyte)
endif()

# --- test labels --------------------------------------------------------------
# Tests can only be labelled from the directory that created them, so each
# component appends its own label (lib / cli / app). Result: one ctest entry
# point with `ctest -L lib|cli|app` slices.

# --- umbrella static analysis -------------------------------------------------
# Components define their own cppcheck targets when cppcheck is on PATH
# (prefixed because OmniNet already owns a plain `cppcheck` target). This
# umbrella runs whichever exist.
add_custom_target(static_analysis
    COMMENT "Running all available static-analysis targets")
foreach(_sa_target daedalus_cppcheck icarus_cppcheck)
    if(TARGET ${_sa_target})
        add_dependencies(static_analysis ${_sa_target})
    endif()
endforeach()
