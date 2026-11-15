cmake_minimum_required(VERSION 3.28)
project(monkeybyte VERSION 0.1.0 LANGUAGES CXX)

# App version: latest git tag (e.g. "0.5.1"); fall back to the project
# version above when git/tags are unavailable (source tarball builds).
# §Monorepo (2026-09-24): ONE version stream — monkeybyte takes its version
# from the same bare x.y.z tags as the library and icarus.
# PROJECT_VERSION fallback covers tarball builds with no git.
find_package(Git QUIET)
set(MONKEYBYTE_APP_VERSION "${PROJECT_VERSION}")
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MONKEYBYTE_GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(MONKEYBYTE_GIT_TAG MATCHES "^v?[0-9]+\\.[0-9]+\\.[0-9]+")
    string(REGEX REPLACE "^v" "" MONKEYBYTE_APP_VERSION "${MONKEYBYTE_GIT_TAG}")
  endif()
endif()
message(STATUS "monkeybyte version: ${MONKEYBYTE_APP_VERSION}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
pkg_check_modules(SOURCEVIEW REQUIRED IMPORTED_TARGET gtksourceview-5)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(WEBKIT REQUIRED IMPORTED_TARGET webkitgtk-6.0)
pkg_check_modules(MD4C REQUIRED IMPORTED_TARGET md4c)
find_package(nlohmann_json 3.11 REQUIRED)

find_program(CPPCHECK cppcheck)

# --- daedalus agent library (chat backend) ---
# §Monorepo (2026-09-24): dual-mode. In-tree the superbuild root already
# added daedalus/ — use the target directly. Standalone (cmake -S
# monkeybyte) fetches the daedalus monorepo from GitLab; its superbuild
# root builds only the library when it is not the top-level project.
# NEVER patch the fetched copy under build/_deps - fix upstream instead.
if(NOT TARGET daedalus)
  include(FetchContent)
  set(DAEDALUS_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
  set(DAEDALUS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    daedalus
    GIT_REPOSITORY https://gitlab.com/leestripp/daedalus.git
    GIT_TAG        main
    # embedded dependency: build it, but don't install its libs/headers/tools
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(daedalus)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# --- core library (shared by app and tests) ---
file(GLOB_RECURSE MONKEYBYTE_CORE_SOURCES CONFIGURE_DEPENDS src/monkeybyte/*.cpp)
list(REMOVE_ITEM MONKEYBYTE_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/monkeybyte/main.cpp)
if(MONKEYBYTE_CORE_SOURCES)
  add_library(monkeybyte_core STATIC ${MONKEYBYTE_CORE_SOURCES})
  target_include_directories(monkeybyte_core PUBLIC src)
  target_compile_definitions(monkeybyte_core PUBLIC MONKEYBYTE_VERSION="${MONKEYBYTE_APP_VERSION}")
  target_link_libraries(monkeybyte_core PUBLIC
    PkgConfig::GTKMM PkgConfig::SOURCEVIEW PkgConfig::CURL PkgConfig::SQLITE3
    PkgConfig::PIPEWIRE
    PkgConfig::WEBKIT PkgConfig::MD4C md4c-html
    nlohmann_json::nlohmann_json
    daedalus)
  set(MONKEYBYTE_CORE_LIB monkeybyte_core)
else()
  set(MONKEYBYTE_CORE_LIB "")
endif()

# --- application ---
add_executable(monkeybyte src/monkeybyte/main.cpp)
target_link_libraries(monkeybyte PRIVATE ${MONKEYBYTE_CORE_LIB}
  PkgConfig::GTKMM PkgConfig::SOURCEVIEW)

# --- GSettings schema compiled into build dir (no system install) ---
find_program(GLIB_COMPILE_SCHEMAS glib-compile-schemas REQUIRED)
set(SCHEMA_BUILD_DIR ${CMAKE_BINARY_DIR}/gschemas)
file(MAKE_DIRECTORY ${SCHEMA_BUILD_DIR})
add_custom_command(
  OUTPUT ${SCHEMA_BUILD_DIR}/gschemas.compiled
  COMMAND ${CMAKE_COMMAND} -E copy
          ${CMAKE_CURRENT_SOURCE_DIR}/data/io.github.monkeybyte.gschema.xml
          ${SCHEMA_BUILD_DIR}/
  COMMAND ${GLIB_COMPILE_SCHEMAS} ${SCHEMA_BUILD_DIR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/io.github.monkeybyte.gschema.xml
  COMMENT "Compiling GSettings schema")
add_custom_target(gschemas ALL DEPENDS ${SCHEMA_BUILD_DIR}/gschemas.compiled)
add_dependencies(monkeybyte gschemas)

# --- theme / style-scheme data next to the binary ---
add_custom_command(TARGET monkeybyte POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:monkeybyte>/data)

# --- install (user-local friendly: cmake --install build --prefix ~/.local) ---
# Default the prefix to the current user's ~/.local when none is given, so a
# bare `cmake --install build` / `ninja install` works unprivileged (podman
# containers run as an arbitrary non-root user). Root keeps the system
# default; an explicit prefix always wins.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND NOT "$ENV{USER}" STREQUAL "root")
  set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local" CACHE PATH "Install prefix" FORCE)
endif()
include(GNUInstallDirs)
install(TARGETS monkeybyte RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/monkeybyte/data
        PATTERN "*.gschema.xml" EXCLUDE)
install(FILES data/io.github.monkeybyte.gschema.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas)
install(FILES resources/monkeybyte.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES resources/monkeybyte.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
# Compile the schema at the install prefix; under ~/.local this needs no sudo
# (GSettings searches XDG data dirs, which include ~/.local/share).
install(CODE "execute_process(COMMAND \"${GLIB_COMPILE_SCHEMAS}\"
          \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas\")")

# --- tests ---
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
if(TEST_SOURCES)
  add_executable(unit_tests ${TEST_SOURCES})
  target_link_libraries(unit_tests PRIVATE ${MONKEYBYTE_CORE_LIB})
  add_test(NAME unit_tests COMMAND unit_tests)
  # §Monorepo (2026-09-24): "app" label for `ctest -L app` slices.
  set_tests_properties(unit_tests PROPERTIES LABELS app TIMEOUT 60)
endif()

# --- static analysis ---
# (named monkeybyte_cppcheck because OmniNet already defines a `cppcheck` target)
if(CPPCHECK)
  file(GLOB_RECURSE ALL_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h tests/*.cpp)
  add_custom_target(monkeybyte_cppcheck
    COMMAND ${CPPCHECK} --std=c++23 --language=c++ --enable=warning,style,performance
            --error-exitcode=1 --quiet -I src --inline-suppr
            # cppcheck has no GTK include paths; define the version-check macro
            # (as true) so #if GTK_CHECK_VERSION(...) guards are evaluable.
            "-DGTK_CHECK_VERSION(major,minor,micro)=1"
            "-DG_GNUC_BEGIN_IGNORE_DEPRECATIONS="
            "-DG_GNUC_END_IGNORE_DEPRECATIONS="
            --suppress=unusedStructMember
            --suppress=constParameterCallback
            --suppress=passedByValueCallback
            --suppress=useStlAlgorithm
            ${ALL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running cppcheck"
    VERBATIM)
endif()
