# demos/daedalus-subagent/CMakeLists.txt
#
# Phase 7 demo: proves the public <daedalus.h> API supports the
# "sub-agent" composition pattern WITHOUT a `--sub-agent` flag on the
# Agent itself. The demo:
#   - creates a parent Agent whose only tool is `spawn_subagent`
#   - that tool constructs a child Agent, runs one turn, returns the
#     child's final text as the tool result
#   - the parent has no idea it just called another Agent; to it,
#     spawn_subagent is a regular tool
#
# This is the canonical "modes are demos" example. Strictly public API.

add_executable(daedalus-subagent
    main.cpp
)

target_link_libraries(daedalus-subagent
    PRIVATE
        daedalus          # libdaedalus.a
)

target_include_directories(daedalus-subagent
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/demos/common   # §Phase 9.10: ThinkingSpinner.h
)

install(TARGETS daedalus-subagent
    RUNTIME DESTINATION "bin"
)
