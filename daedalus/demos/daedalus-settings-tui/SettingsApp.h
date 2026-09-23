// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/SettingsApp.h
//
// Shared context and helpers for the daedalus-settings-tui tabs. The app
// edits the SQLite settings store through the public
// <daedalus/settings.h> API only — no Agent is ever constructed.

#pragma once

#include <daedalus/settings.h>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <functional>
#include <iosfwd>
#include <string>
#include <vector>

namespace settings_tui {

/// Shared state handed to every tab. `store` is a lightweight copyable
/// handle (the underlying SQLite connection lives in the library's
/// DbPool). `status` is the one-line message shown in the footer.
struct AppContext
{
    daedalus::SettingsStore store;
    std::string status;

    /// Per-tab keyboard shortcut handlers, registered by the tab
    /// factories in tab order (Providers, Models, MCP, Agent). The root
    /// CatchEvent routes events to `key_handlers[active_tab]` — FTXUI
    /// only dispatches events down the focused child chain, so
    /// tab-local CatchEvents never see keys while the header toggle has
    /// focus.
    std::vector<std::function<bool(ftxui::Event)>> key_handlers;

    /// Per-tab "a modal dialog is open" checks, same ordering. While a
    /// modal is open the root CatchEvent must leave navigation keys
    /// (arrows, Esc) alone so the modal can use them.
    std::vector<std::function<bool()>> modal_checks;

    void set_status(std::string msg) { status = std::move(msg); }
};

// -- Tab factories (implemented in <Name>Tab.cpp) -------------------------

ftxui::Component MakeProvidersTab(AppContext& ctx);
ftxui::Component MakeModelsTab(AppContext& ctx);
ftxui::Component MakeMcpTab(AppContext& ctx);
ftxui::Component MakeAgentTab(AppContext& ctx);

// -- App entry points ------------------------------------------------------

/// Run the fullscreen interactive TUI. Blocks until the user quits.
int RunApp(daedalus::SettingsStore store);

/// Non-interactive dump of every settings section (secrets masked).
int DumpSettings(daedalus::SettingsStore& store, std::ostream& out);

// -- Small shared helpers (SettingsApp.cpp) --------------------------------

/// Mask a secret for display: "" -> "(not set)", short -> "****",
/// otherwise "<first 3>…<last 4>".
std::string MaskSecret(const std::string& secret);

/// Strict integer parse (whole string must be consumed).
bool ParseInt(const std::string& s, int& out);
bool ParseSize(const std::string& s, size_t& out);
bool ParseDouble(const std::string& s, double& out);

/// Split on ASCII whitespace (used for MCP args).
std::vector<std::string> SplitWords(const std::string& s);

/// Join with a single space (inverse of SplitWords).
std::string JoinWords(const std::vector<std::string>& words);

/// Parse "KEY=VALUE" lines into a map (MCP env editor format).
std::map<std::string, std::string> ParseEnvLines(const std::string& s);

/// Render a map back into "KEY=VALUE" lines.
std::string EnvToLines(const std::map<std::string, std::string>& env);

/// Build a single-line labelled Input row component:
/// "label [................]".
/// If `on_enter` is given, pressing Enter inside the field invokes it
/// (used to submit the surrounding form — FTXUI Inputs consume Return
/// and arrow keys, so field-level submit is the reliable keyboard path).
ftxui::Component FormRow(const std::string& label,
                         std::string* content,
                         bool password = false,
                         std::function<void()> on_enter = nullptr);

} // namespace settings_tui
