# demos/example-tools/CMakeLists.txt
#
# Non-core tools moved out of libdaedalus so the default tool surface is
# small. Demos that need file/git/search/network/etc. tools link this
# static library and register the individual tools they want through
# ToolHandler::get_tool_factory().

set(EXAMPLE_TOOLS_SOURCES
    BackgroundTool.cpp
    GitTool.cpp
    NetworkTool.cpp
    PDFTool.cpp
    SearchTool.cpp
    SystemInfoTool.cpp
)

add_library(daedalus_example_tools STATIC ${EXAMPLE_TOOLS_SOURCES})

target_include_directories(daedalus_example_tools
    PUBLIC
        ${CMAKE_SOURCE_DIR}/demos/example-tools
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/include
        ${nlohmann_json_INCLUDE_DIR}
        ${POPPLER_INCLUDE_DIRS}
)

target_link_libraries(daedalus_example_tools
    PUBLIC
        daedalus
    PRIVATE
        OmniNet
        ${POPPLER_LIBRARIES}
)
