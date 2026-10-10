# demos/daedalus-settings-tui/CMakeLists.txt
#
# FTXUI-based TUI settings editor for the daedalus SQLite settings store.
# Edits providers, models, MCP servers and agent kv settings through the
# public <daedalus/settings.h> API only — it never constructs an Agent.

include(FetchContent)

set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG        v7.0.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(ftxui)

add_executable(daedalus-settings-tui
    main.cpp
    SettingsApp.cpp
    ProvidersTab.cpp
    ModelsTab.cpp
    McpTab.cpp
    AgentTab.cpp
    WebSearchTab.cpp
)

target_link_libraries(daedalus-settings-tui
    PRIVATE
        daedalus
        ftxui::screen
        ftxui::dom
        ftxui::component
)

target_include_directories(daedalus-settings-tui
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

install(TARGETS daedalus-settings-tui
    RUNTIME DESTINATION "bin"
)
