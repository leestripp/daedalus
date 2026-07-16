# demos/daedalus-cli/CMakeLists.txt
#
# Phase 6 demo: the historical CLI binary, now using ONLY the public
# <daedalus.h> API. The demo links against libdaedalus.a and the sink is
# the host's responsibility — it includes no internal headers.
#
# This is the canonical "embedding" example referenced by
# docs/embedding.md. Hosts that want to embed daedalus in their own
# application copy this layout: <daedalus.h> + libdaedalus.a + their
# own AgentSink subclass.

add_executable(daedalus-cli
    main.cpp
    ConsoleAgentSink.cpp
    migrate.cpp    # Plan.md Phase 5: --migrate-data subcommand
)

# ACP sub-library — Agent Communication Protocol C++ client.
# Optional because ACP is still a stub (SSE streaming, error handling,
# status enum all incomplete). Default OFF until stable.
# See CodeMax.md "ACP — code review (2026-07-16)" for the task list.
option(DAEDALUS_CLI_ACP "Build ACP client sub-library into daedalus-cli" OFF)
if(DAEDALUS_CLI_ACP)
    # Build the ACP tests when ACP itself is being built. These are tiny
    # smoke tests; doctest isn't required (the existing test_acp_basic.cpp
    # uses raw assert + std::cout).
    set(ACP_BUILD_TESTS ON CACHE BOOL "Build ACP tests" FORCE)
    add_subdirectory(acp)
endif()

target_link_libraries(daedalus-cli
    PRIVATE
        daedalus          # the static library under test
)

# §Phase 6: the CLI demo no longer reaches into src/. The build is
# strictly public-API ↔ library-static-lib.
target_include_directories(daedalus-cli
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include   # for <daedalus.h>
        ${CMAKE_SOURCE_DIR}/demos/common  # §Phase 9.10: ThinkingSpinner.h
)

# Ship as `daedalus-cli` in ~/.local/bin/ — its post-refactor name.
install(TARGETS daedalus-cli
    RUNTIME DESTINATION "bin"
)