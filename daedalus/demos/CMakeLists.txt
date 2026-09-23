# demos/CMakeLists.txt
#
# Phase 1+ layout: all standalone executables that exercise libdaedalus
# live under demos/. Each one links against the static library and is
# independently testable.
#
# Right now there is one demo: daedalus-cli (the historical CLI binary,
# preserved as the regression test harness). Future demos (daedalus-tui,
# daedalus-web, daedalus-gtk4) will be added here too, each behind its
# own CMake option so the default build stays small.

# Default: build all demos. Set DAEDALUS_BUILD_DEMOS=OFF to skip them
# entirely (useful for embedders who only want libdaedalus.a).
option(DAEDALUS_BUILD_DEMOS "Build demo executables" ON)

if(NOT DAEDALUS_BUILD_DEMOS)
    return()
endif()

# §TOOL-TRIM (prompt.txt): non-core tools live here as examples. Demos
# that need file / git / search / pdf / network tools link the resulting
# static library and register the individual tools they want.
add_subdirectory(example-tools)

# Always-on demo: the historical CLI binary, preserved as the
# regression test that the full feature set still works through the
# static-library boundary.
add_subdirectory(daedalus-cli)

# Phase 7 demos: composition patterns, each behind a CMake option so
# the default build stays small. The CLI demo is always-on because it's
# the regression harness; the rest are off by default.
option(DAEDALUS_DEMO_SUBAGENT "Build sub-agent demo"  ON)
if(DAEDALUS_DEMO_SUBAGENT)
    add_subdirectory(daedalus-subagent)
endif()

option(DAEDALUS_DEMO_MANAGER "Build manager demo"    ON)
if(DAEDALUS_DEMO_MANAGER)
    add_subdirectory(daedalus-manager)
endif()

# Phase 7 webui: HTTP + SSE host for libdaedalus.a. Uses OmniNet
# (already linked into daedalus as a PUBLIC dep). Off by default to
# keep the default build slim.
option(DAEDALUS_DEMO_WEBUI "Build webui demo"      OFF)
if(DAEDALUS_DEMO_WEBUI)
    add_subdirectory(daedalus-webui)
endif()

# Settings TUI: FTXUI-based editor for the SQLite SettingsStore
# (providers, models, MCP servers, agent kv settings). FTXUI is fetched
# into build/_deps/ like the other CMake-managed dependencies.
option(DAEDALUS_DEMO_SETTINGS_TUI "Build settings TUI demo" ON)
if(DAEDALUS_DEMO_SETTINGS_TUI)
    add_subdirectory(daedalus-settings-tui)
endif()
