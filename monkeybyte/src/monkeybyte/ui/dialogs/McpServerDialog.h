#pragma once

#include "monkeybyte/backend/McpBridge.h" // McpServer (UI-level model)

#include <gtkmm.h>
#include <optional>
#include <sigc++/sigc++.h>

namespace monkeybyte {

// §5.4 — create or edit an MCP server. Env is a JSON object; invalid JSON
// keeps the dialog open with an inline error. Reads/writes the daedalus
// settings registry (the one the agent loads) via mcp_bridge.
class McpServerDialog : public Gtk::Window {
public:
    explicit McpServerDialog(Gtk::Window& parent,
                             const std::optional<McpServer>& existing = std::nullopt);

    sigc::signal<void()> signal_saved() { return signal_saved_; }

private:
    void on_save();

    Gtk::Entry name_, command_;
    Gtk::TextView env_;
    Gtk::Switch enabled_;
    Gtk::Label error_;

    sigc::signal<void()> signal_saved_;
};

} // namespace monkeybyte
