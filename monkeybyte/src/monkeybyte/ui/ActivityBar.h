#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <vector>

namespace monkeybyte {

// Leftmost 48 px icon column (design §3). Owns 4 toggle buttons; exactly one
// is active at a time.
class ActivityBar : public Gtk::Box {
public:
    enum Panel { Workspace = 0, Search = 1, Git = 2, Settings = 3 };

    ActivityBar();

    void set_active(Panel p);
    Panel active() const { return active_; }

    sigc::signal<void(Panel)> signal_panel_selected() { return signal_panel_selected_; }

private:
    struct IconButton {
        Gtk::Button button;
        Panel panel;
    };

    // icon_name: symbolic icon in data/icons (recolored by CSS); fallback:
    // emoji label used when the icon file is missing.
    Gtk::Button& add_button(const Glib::ustring& icon_name,
                            const Glib::ustring& fallback,
                            const Glib::ustring& tooltip, Panel panel, bool bottom);

    std::vector<std::unique_ptr<IconButton>> buttons_;
    Panel active_ = Workspace;
    sigc::signal<void(Panel)> signal_panel_selected_;
};

} // namespace monkeybyte
