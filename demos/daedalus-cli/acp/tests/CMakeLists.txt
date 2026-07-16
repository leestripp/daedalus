# ACP Tests CMakeLists.txt

# Find the doctest header that the main tests/CMakeLists.txt already
# fetched into the shared _deps tree. ACP tests use doctest for the
# same reporting style as the rest of the codebase.
find_path(DOCTEST_INCLUDE_DIR
    NAMES doctest/doctest.h
    PATHS ${CMAKE_BINARY_DIR}/_deps/doctest-src
    NO_DEFAULT_PATH
)

# Basic test
add_executable(test_acp_basic test_acp_basic.cpp)
target_link_libraries(test_acp_basic PRIVATE
    acp                  # the ACP client lib
    ${UUID_LIBRARIES}    # libuuid for uuid_generate_random / uuid_unparse_lower
)
target_include_directories(test_acp_basic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include          # <daedalus/net/SseParser.h> for §ACP-7
    ${DOCTEST_INCLUDE_DIR}                # doctest/doctest.h
)

add_test(NAME test_acp_basic COMMAND test_acp_basic)