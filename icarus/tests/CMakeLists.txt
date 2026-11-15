# ============================================================================
# tests/ — icarus unit tests (doctest + CTest)
#
# §Tests 2026-08-20: a top-level tests/ folder previously existed only as
# untracked working-tree files and was lost in a `git restore` cleanup.
# These tests are COMMITTED so that can never happen again.
#
# Coverage:
#   test_cli_args.cpp      — parse_cli_args() flag parser (src/util.h)
#   test_system_prompt.cpp — resolve_system_prompt() priority chain (src/util.h)
#   test_migrate.cpp       — migration plan + argv detection (src/migrate.cpp)
# ============================================================================

# Same doctest tag daedalus uses (v2.5.3) so both trees agree.
FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.5.3
    GIT_SHALLOW    TRUE
)
set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF CACHE BOOL "" FORCE)
set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(doctest)

# Include dirs needed to compile src/util.h / src/migrate.h — mirrors the
# icarus target (daedalus headers pull OmniNet/nlohmann-json headers
# transitively). §Monorepo (2026-09-24): dep include dirs come from the
# parent's DAEDALUS_DEP_INCLUDES, computed for both build modes.
set(ICARUS_TEST_INCLUDES
    ${icarus_SOURCE_DIR}/src
    ${DAEDALUS_SRC_DIR}/include
    ${DAEDALUS_SRC_DIR}/demos/common
    ${DAEDALUS_DEP_INCLUDES}
    ${PNG_INCLUDE_DIRS}
    ${UUID_INCLUDE_DIRS}
)

# --- parse_cli_args / resolve_system_prompt (header-only, no lib link) ---
add_executable(icarus_test_util
    test_cli_args.cpp
    test_system_prompt.cpp
)
target_include_directories(icarus_test_util PRIVATE ${ICARUS_TEST_INCLUDES})
target_link_libraries(icarus_test_util PRIVATE doctest::doctest)

# --- migrate: needs src/migrate.cpp + libdaedalus.a (Paths::make_default) ---
add_executable(icarus_test_migrate
    test_migrate.cpp
    ${icarus_SOURCE_DIR}/src/migrate.cpp
)
# §Monorepo (2026-09-24): link the `daedalus` CMake target in both build
# modes (it carries OmniNet/OpenSSL/nlohmann_json transitively) instead of
# the old ExternalProject .a paths.
target_include_directories(icarus_test_migrate PRIVATE ${ICARUS_TEST_INCLUDES})
target_link_libraries(icarus_test_migrate
    PRIVATE
        doctest::doctest
        daedalus
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

add_test(NAME cli_args      COMMAND icarus_test_util --test-case="cli_args*")
add_test(NAME system_prompt COMMAND icarus_test_util --test-case="system_prompt*")
add_test(NAME migrate       COMMAND icarus_test_migrate)

# §Monorepo (2026-09-24): label icarus tests "cli" for `ctest -L cli`
# slices from the monorepo root build.
get_property(_icarus_all_tests DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY TESTS)
if(_icarus_all_tests)
    set_property(TEST ${_icarus_all_tests} APPEND PROPERTY LABELS cli)
    set_property(TEST ${_icarus_all_tests} PROPERTY TIMEOUT 60)
endif()
