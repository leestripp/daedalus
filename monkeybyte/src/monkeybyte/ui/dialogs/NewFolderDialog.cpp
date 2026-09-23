#include "monkeybyte/ui/dialogs/NewFolderDialog.h"

#include <filesystem>

namespace monkeybyte {

NewFolderDialog::NewFolderDialog(Gtk::Window& parent, const std::string& root,
                                 const std::string& initial_dir)
    : picker_(root, initial_dir) {
    set_title("Create New Folder");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(520, 520);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(12);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* title = Gtk::make_managed<Gtk::Label>("Create New Folder");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close = Gtk::make_managed<Gtk::Button>("×");
    close->set_has_frame(false);
    close->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close);

    auto* name_label = Gtk::make_managed<Gtk::Label>("Folder name:");
    name_label->set_halign(Gtk::Align::START);
    name_entry_.set_hexpand(true);
    name_entry_.set_placeholder_text("my-new-folder");
    name_entry_.signal_changed().connect([this] {
        if (create_btn_)
            create_btn_->set_sensitive(!name_entry_.get_text().empty() &&
                                       !picker_.root_empty());
    });
    name_entry_.signal_activate().connect([this] { on_create(); });

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    create_btn_ = Gtk::make_managed<Gtk::Button>("Create");
    create_btn_->add_css_class("suggested-action");
    create_btn_->set_sensitive(false); // until a name is typed
    create_btn_->signal_clicked().connect([this] { on_create(); });
    footer->append(*cancel);
    footer->append(*create_btn_);

    picker_.set_vexpand(true);
    body->append(*header);
    body->append(picker_);
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

void NewFolderDialog::on_create() {
    if (picker_.root_empty()) return;

    const std::string folder = picker_.chosen_dir();
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        picker_.set_status("Error: not a directory: " + folder);
        return;
    }
    if (!picker_.within_root(folder)) {
        picker_.set_status("Error: folder is outside the workspace.");
        return;
    }

    const auto name = std::string(name_entry_.get_text());
    if (name.empty()) {
        picker_.set_status("Enter a folder name.");
        return;
    }
    if (name.find('/') != std::string::npos || name == "." || name == "..") {
        picker_.set_status("Name must be a plain folder name (no path).");
        return;
    }

    signal_folder_named_.emit(folder, name);
    hide();
}

} // namespace monkeybyte
