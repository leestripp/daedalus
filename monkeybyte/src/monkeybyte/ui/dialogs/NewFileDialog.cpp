#include "monkeybyte/ui/dialogs/NewFileDialog.h"

#include "monkeybyte/AppServices.h"

#include <filesystem>

namespace monkeybyte {

namespace fs = std::filesystem;

NewFileDialog::NewFileDialog(Gtk::Window& parent, AppServices& services,
                             const std::string& initial_dir)
    : services_(services),
      picker_(services_.files.workspace().empty()
                  ? std::string()
                  : services_.files.workspace().string(),
              initial_dir) {
    set_title("Create New File");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(520, 520);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(12);

    // ── header ─────────────────────────────────────────────────────────
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* title = Gtk::make_managed<Gtk::Label>("Create New File");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close_btn);

    // ── filename entry ─────────────────────────────────────────────────
    filename_entry_.set_hexpand(true);
    filename_entry_.set_placeholder_text("filename.ext");
    filename_entry_.signal_activate().connect([this] { on_create(); });

    // ── footer ─────────────────────────────────────────────────────────
    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    create_btn_ = Gtk::make_managed<Gtk::Button>("Create");
    create_btn_->add_css_class("suggested-action");
    create_btn_->signal_clicked().connect([this] { on_create(); });
    footer->append(*cancel);
    footer->append(*create_btn_);

    picker_.set_vexpand(true);
    body->append(*header);
    body->append(picker_);
    body->append(filename_entry_);
    body->append(*footer);
    set_child(*body);

    // Escape closes.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { hide(); return true; }
            return false;
        },
        false);
    add_controller(keys);

    if (picker_.root_empty())
        create_btn_->set_sensitive(false);
}

void NewFileDialog::on_create() {
    if (picker_.root_empty()) return;

    const std::string folder = picker_.chosen_dir();
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        picker_.set_status("Error: not a directory: " + folder);
        return;
    }
    if (!picker_.within_root(folder)) {
        picker_.set_status("Error: folder is outside the workspace.");
        return;
    }

    const auto filename = filename_entry_.get_text();
    if (filename.empty()) {
        picker_.set_status("Enter a file name.");
        return;
    }

    const auto target = (fs::path(folder) / std::string(filename)).string();
    auto result = services_.files.write(target, "");
    if (!result) {
        picker_.set_status("Error: " + result.error());
        return;
    }

    signal_file_created_.emit(target);
    hide();
}

} // namespace monkeybyte
