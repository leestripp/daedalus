# demos/daedalus-manager/CMakeLists.txt
#
# Phase 7 demo: 3-Agent orchestration pattern. Three daedalus::Agent
# instances (planner, actor, validator) share `<project>/.daedalus/
# messages.json` and are driven sequentially by the host loop. The
# validator's verdict ("VERDICT: APPROVED" or "VERDICT: REJECTED")
# drives the host's retry decision.
#
# Strictly public <daedalus.h> API.

add_executable(daedalus-manager
    main.cpp
)

target_link_libraries(daedalus-manager
    PRIVATE
        daedalus
)

target_include_directories(daedalus-manager
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/demos/common   # §Phase 9.10: ThinkingSpinner.h
)

install(TARGETS daedalus-manager
    RUNTIME DESTINATION "bin"
)
