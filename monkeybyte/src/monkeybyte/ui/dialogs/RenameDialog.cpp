#include "monkeybyte/ui/dialogs/RenameDialog.h"

namespace monkeybyte {

RenameDialog::RenameDialog(Gtk::Window& parent, const std::string& current_name,
                           bool is_dir) {
    const std::string kind = is_dir ? "Folder" : "File";
    set_title("Rename " + kind);
    set_transient_for(parent);
    set_modal(true);
    set_default_size(360, -1);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>("Rename " + kind);
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close = Gtk::make_managed<Gtk::Button>("×");
    close->set_has_frame(false);
    close->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close);

    auto* name_label = Gtk::make_managed<Gtk::Label>("New name:");
    name_label->set_halign(Gtk::Align::START);
    name_entry_.set_hexpand(true);
    name_entry_.set_text(current_name);
    name_entry_.select_region(0, -1);
    name_entry_.signal_activate().connect([this] { on_rename(); });

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    auto* rename = Gtk::make_managed<Gtk::Button>("Rename");
    rename->add_css_class("suggested-action");
    rename->signal_clicked().connect([this] { on_rename(); });
    footer->append(*cancel);
    footer->append(*rename);

    body->append(*header);
    body->append(*name_label);
    body->append(name_entry_);
    body->append(*footer);
    set_child(*body);

    // Escape closes (§5).
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { hide(); return true; }
            return false;
        },
        false);
    add_controller(keys);
}

void RenameDialog::on_rename() {
    const auto name = name_entry_.get_text();
    if (name.empty()) return;
    signal_renamed_.emit(name);
    hide();
}

} // namespace monkeybyte
