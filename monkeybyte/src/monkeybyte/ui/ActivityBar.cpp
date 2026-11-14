#include "monkeybyte/ui/ActivityBar.h"
#include "monkeybyte/ui/theme/ThemeManager.h"

namespace monkeybyte {

ActivityBar::ActivityBar() : Gtk::Box(Gtk::Orientation::VERTICAL) {
    add_css_class("activity-bar");
    set_size_request(48, -1);

    // App-private symbolic icons (design §3): GTK recolors *-symbolic SVGs
    // to the button's CSS foreground color (muted → hover → accent active).
    // The search path is registered in main() before any window exists.

    add_button("mb-workspace-symbolic", "📁", "Workspace", Workspace, false);
    add_button("mb-search-symbolic", "🔍", "Search", Search, false);
    add_button("mb-source-control-symbolic", "🌿", "Source Control", Git, false);

    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_vexpand(true);
    append(*spacer);

    add_button("mb-settings-symbolic", "⚙️", "Settings", Settings, true);

    set_active(Workspace);
}

Gtk::Button& ActivityBar::add_button(const Glib::ustring& icon_name,
                                     const Glib::ustring& fallback,
                                     const Glib::ustring& tooltip, Panel panel,
                                     bool /*bottom*/) {
    auto ib = std::make_unique<IconButton>();
    ib->panel = panel;

    const std::string path =
        ThemeManager::data_dir() + "/icons/" + icon_name + ".svg";
    if (Glib::file_test(path, Glib::FileTest::EXISTS)) {
        auto* image = Gtk::make_managed<Gtk::Image>();
        image->set_from_icon_name(icon_name);
        image->set_pixel_size(20);
        ib->button.set_child(*image);
    } else {
        ib->button.set_label(fallback);
    }

    ib->button.set_tooltip_text(tooltip);
    ib->button.set_size_request(40, 40);
    ib->button.set_has_frame(false);
    auto* btn = &ib->button;
    ib->button.signal_clicked().connect([this, panel, btn] {
        set_active(panel);
        signal_panel_selected_.emit(panel);
    });
    append(ib->button);
    buttons_.push_back(std::move(ib));
    return *btn;
}

void ActivityBar::set_active(Panel p) {
    active_ = p;
    for (const auto& ib : buttons_) {
        if (ib->panel == p)
            ib->button.add_css_class("active");
        else
            ib->button.remove_css_class("active");
    }
}

} // namespace monkeybyte
