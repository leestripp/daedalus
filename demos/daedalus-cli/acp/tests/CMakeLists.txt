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

# End-to-end test: spins up an in-process OmniNet HTTP server that
# emits the 11 spec ACP event types via SSE, then connects with
# AcpClient and verifies the right IRunEventHandler callbacks fire.
# Closes out item 2 from the "what's next" list.
add_executable(test_acp_end_to_end test_acp_end_to_end.cpp)
target_link_libraries(test_acp_end_to_end PRIVATE
    acp                       # the ACP client lib
    OmniNet                   # in-process HTTP server for the mock
    ${UUID_LIBRARIES}         # libuuid (create_run generates UUIDs)
    # DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN in test_acp_end_to_end.cpp
    # generates main() from the header — no need to link libdoctest.
    pthread
)
target_include_directories(test_acp_end_to_end PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/include          # <daedalus/net/SseParser.h>
    ${DOCTEST_INCLUDE_DIR}                # doctest/doctest.h
)

add_test(NAME test_acp_end_to_end COMMAND test_acp_end_to_end)
# The end-to-end test binds a TCP port. Bump the timeout to give the
# server time to bind + the client to connect + the SSE lifecycle to play.
set_tests_properties(test_acp_end_to_end PROPERTIES TIMEOUT 30)