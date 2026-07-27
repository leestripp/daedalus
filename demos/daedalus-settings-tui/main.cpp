// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/main.cpp
//
// daedalus-settings-tui — interactive TUI editor for the daedalus
// SQLite settings store (providers, models, MCP servers, agent options).
//
// Usage:
//   daedalus-settings-tui [--home <path>] [--dump] [--help]
//
//   --home <path>  Edit the per-agent store at <path>/daedalus.db
//                  (SettingsStore::for_home) instead of the global one.
//   --dump         Print all settings (secrets masked) and exit.
//   --help         Show this help.

#include "SettingsApp.h"

#include <daedalus/settings.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_help(std::ostream& out)
{
    out << "daedalus-settings-tui — edit the daedalus settings store\n"
        << "\n"
        << "Usage: daedalus-settings-tui [--home <path>] [--dump] [--help]\n"
        << "\n"
        << "Options:\n"
        << "  --home <path>  Edit the per-agent store at <path>/daedalus.db\n"
        << "                 instead of the global user store.\n"
        << "  --dump         Print all settings (secrets masked) and exit.\n"
        << "  --help         Show this help.\n"
        << "\n"
        << "Keys (entity tabs): a = add, e / Enter = edit, d = delete,\n"
        << "s = set default model (Models tab), Esc = close dialog,\n"
        << "Left/Right = switch section, Ctrl+C = quit.\n"
        << "Inside a form, Enter in a field saves the form.\n";
}

} // namespace

int main(int argc, char** argv)
{
    std::filesystem::path home;
    bool dump = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_help(std::cout);
            return 0;
        }
        if (arg == "--dump") {
            dump = true;
            continue;
        }
        if (arg == "--home") {
            if (i + 1 >= argc) {
                std::cerr << "error: --home requires a path argument\n";
                return 2;
            }
            home = argv[++i];
            continue;
        }
        std::cerr << "error: unknown argument '" << arg << "' (try --help)\n";
        return 2;
    }

    if (!home.empty()) {
        auto store = daedalus::SettingsStore::for_home(home);
        if (dump) return settings_tui::DumpSettings(store, std::cout);
        return settings_tui::RunApp(std::move(store));
    }

    auto& store = daedalus::SettingsStore::instance();
    if (dump) return settings_tui::DumpSettings(store, std::cout);
    return settings_tui::RunApp(store);
}
