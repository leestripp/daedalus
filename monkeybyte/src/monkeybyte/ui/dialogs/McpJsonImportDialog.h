#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>

namespace monkeybyte {

// §5.4 — add one or more MCP servers by pasting the raw JSON that MCP
// providers publish ("mcpServers" wrapper, single keyed object, or bare
// single-server config). Invalid JSON keeps the dialog open with an
// inline error. Parsed servers are upserted into the daedalus settings
// registry (the one the agent loads).
class McpJsonImportDialog : public Gtk::Window {
public:
    explicit McpJsonImportDialog(Gtk::Window& parent);

    sigc::signal<void()> signal_saved() { return signal_saved_; }

private:
    void on_import();

    Gtk::TextView json_;
    Gtk::Label error_;

    sigc::signal<void()> signal_saved_;
};

} // namespace monkeybyte
