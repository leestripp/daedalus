# demos/daedalus-cli/CMakeLists.txt
#
# Phase 6 demo: the historical CLI binary. The demo links against
# libdaedalus.a and the sink is the host's responsibility. It uses the
# public <daedalus.h> API for the Agent lifecycle and internal
# ToolHandler/ToolFactory headers only to register the individual
# example tools it opts in to (file, git, search, etc.).

add_executable(daedalus-cli
    main.cpp
    ConsoleAgentSink.cpp
    migrate.cpp    # Plan.md Phase 5: --migrate-data subcommand
    A2aAgentBackend.cpp
)

target_link_libraries(daedalus-cli
    PRIVATE
        daedalus                    # the static library under test
        daedalus_example_tools      # §TOOL-TRIM: file / git / search / etc.
)

target_include_directories(daedalus-cli
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include              # for <daedalus.h>
        ${PROJECT_SOURCE_DIR}/src                  # for tools/ToolHandler.h
        ${PROJECT_SOURCE_DIR}/src/tools            # for ToolFactory.h
        ${PROJECT_SOURCE_DIR}/demos/common         # §Phase 9.10: ThinkingSpinner.h
        ${PROJECT_SOURCE_DIR}/demos/example-tools  # for example_tools.h
)

# Ship as `daedalus-cli` in ~/.local/bin/ — its post-refactor name.
install(TARGETS daedalus-cli
    RUNTIME DESTINATION "bin"
)