#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>

namespace monkeybyte {

struct AppServices;

// §4.4 — read-only git status (branch pill + change rows).
class GitPanel : public Gtk::Box {
public:
    explicit GitPanel(AppServices& services);

    void refresh(); // re-query git status; call when the panel is shown

    sigc::signal<void(const std::string&, int)> signal_open_file() {
        return signal_open_file_;
    }

private:
    void set_status(const Glib::ustring& text);

    AppServices& services_;
    Gtk::ScrolledWindow scroller_;
    Gtk::Box content_;
    Gtk::Label status_;
    sigc::signal<void(const std::string&, int)> signal_open_file_;
};

} // namespace monkeybyte
