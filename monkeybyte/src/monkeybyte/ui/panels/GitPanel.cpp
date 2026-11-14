#include "monkeybyte/ui/panels/GitPanel.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/GitService.h"

namespace monkeybyte {

GitPanel::GitPanel(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL),
      services_(services),
      content_(Gtk::Orientation::VERTICAL, 4) {
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    header->add_css_class("sidebar-header");
    auto* title = Gtk::make_managed<Gtk::Label>("SOURCE CONTROL");
    title->set_halign(Gtk::Align::START);
    header->append(*title);
    append(*header);

    content_.set_margin(8);
    status_.add_css_class("muted");
    status_.set_vexpand(true);
    status_.set_wrap(true);
    scroller_.set_vexpand(true);
    append(scroller_);
    set_status("No repository — open a workspace that contains a .git directory.");
}

void GitPanel::set_status(const Glib::ustring& text) {
    if (text.empty()) {
        scroller_.set_child(content_);
    } else {
        status_.set_text(text);
        scroller_.set_child(status_);
    }
}

void GitPanel::refresh() {
    // clear previous rows
    while (auto* child = content_.get_first_child())
        content_.remove(*child);

    if (services_.files.workspace().empty()) {
        set_status("No repository — open a workspace that contains a .git directory.");
        return;
    }

    GitService::Status st;
    try {
        st = GitService::status(services_.files.workspace());
    } catch (const std::exception& e) {
        set_status(Glib::ustring("Error loading git status: ") + e.what());
        return;
    }

    if (!st.is_repo) {
        set_status("No repository — open a workspace that contains a .git directory.");
        return;
    }

    // branch pill (§4.4)
    auto* branch = Gtk::make_managed<Gtk::Label>("🌿 " + st.branch);
    branch->set_halign(Gtk::Align::START);
    branch->add_css_class("monospace");
    content_.append(*branch);

    if (st.changes.empty()) {
        auto* clean = Gtk::make_managed<Gtk::Label>("No changes detected");
        clean->set_halign(Gtk::Align::START);
        clean->add_css_class("muted");
        content_.append(*clean);
    } else {
        for (const auto& c : st.changes) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto* letter = Gtk::make_managed<Gtk::Label>(std::string(1, c.status));
            letter->add_css_class("monospace");
            auto* path = Gtk::make_managed<Gtk::Label>(c.path);
            path->set_halign(Gtk::Align::START);
            path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
            row->append(*letter);
            row->append(*path);

            // porcelain paths are workspace-relative; click opens the file
            const std::string abs =
                (services_.files.workspace() / c.path).string();
            row->set_tooltip_text(abs);
            auto click = Gtk::GestureClick::create();
            click->set_button(GDK_BUTTON_PRIMARY);
            click->signal_pressed().connect(
                [this, abs](int, double, double) {
                    signal_open_file_.emit(abs, 0);
                });
            row->add_controller(click);

            content_.append(*row);
        }
    }
    set_status("");
}

} // namespace monkeybyte
