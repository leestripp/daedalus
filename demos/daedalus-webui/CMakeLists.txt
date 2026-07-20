# demos/daedalus-webui/CMakeLists.txt
#
# Phase 7 demo: HTTP + SSE host for libdaedalus.a.
#
# Uses OmniNet (already a daedalus dependency — see top-level
# CMakeLists.txt — `target_link_libraries(daedalus PUBLIC OmniNet)`).
# OmniNet provides the HTTP server, SSE streaming, and CORS support.
# The demo just links `daedalus`; OmniNet propagates via INTERFACE.

add_executable(daedalus-webui
    main.cpp
)

target_link_libraries(daedalus-webui
    PRIVATE
        daedalus                 # libdaedalus.a (+ OmniNet via PUBLIC dep)
        daedalus_example_tools   # §TOOL-TRIM: file / git / search / etc.
)

target_include_directories(daedalus-webui
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include              # for <daedalus.h>
        ${CMAKE_SOURCE_DIR}/src                  # for tools/ToolHandler.h
        ${CMAKE_SOURCE_DIR}/src/tools            # for ToolFactory.h
        ${CMAKE_SOURCE_DIR}/demos/example-tools  # for example_tools.h
)

install(TARGETS daedalus-webui
    RUNTIME DESTINATION "bin"
)