#include "monkeybyte/ui/dialogs/WorkspacePickerDialog.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/ui/dialogs/NewFolderDialog.h"

#include <cstdlib>
#include <filesystem>

namespace monkeybyte {

namespace {
std::string home_dir() {
    const char* h = std::getenv("HOME");
    return h ? h : "/";
}

// Directory-picker row: folder icon + name (design §4.1).
Gtk::Box* folder_row(const std::string& name) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    box->set_halign(Gtk::Align::START);
    box->set_margin_start(8);
    box->set_margin_top(4);
    box->set_margin_bottom(4);
    auto* icon = Gtk::make_managed<Gtk::Image>();
    icon->set_from_icon_name("mb-folder-symbolic");
    icon->set_pixel_size(16);
    box->append(*icon);
    box->append(*Gtk::make_managed<Gtk::Label>(name));
    return box;
}
} // namespace

WorkspacePickerDialog::WorkspacePickerDialog(Gtk::Window& parent, AppServices& services)
    : services_(services) {
    set_title("Open Workspace");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(520, 480);

    current_dir_ = services_.files.workspace().empty()
        ? home_dir()
        : services_.files.workspace().string();

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(12);

    // ── header: title, new-folder, close ─────────────────────────────
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* title = Gtk::make_managed<Gtk::Label>("Open Workspace");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* new_folder = Gtk::make_managed<Gtk::Button>();
    {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 2);
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("mb-folder-symbolic");
        icon->set_pixel_size(16);
        box->append(*icon);
        box->append(*Gtk::make_managed<Gtk::Label>("+"));
        new_folder->set_child(*box);
    }
    new_folder->set_tooltip_text("Create new folder");
    new_folder->set_has_frame(false);
    new_folder->signal_clicked().connect([this] {
        // Selector rooted at the dir being browsed (§5.2).
        auto* dlg = Gtk::make_managed<NewFolderDialog>(*this, current_dir_,
                                                       current_dir_);
        dlg->signal_folder_named().connect(
            sigc::mem_fun(*this, &WorkspacePickerDialog::on_new_folder));
        dlg->present();
    });
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*new_folder);
    header->append(*close_btn);

    // ── breadcrumb ───────────────────────────────────────────────────
    breadcrumb_.set_halign(Gtk::Align::START);
    breadcrumb_.set_ellipsize(Pango::EllipsizeMode::START);
    breadcrumb_.add_css_class("monospace");

    // ── directory list ───────────────────────────────────────────────
    scroller_.set_vexpand(true);
    scroller_.set_min_content_height(240);
    dir_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    dir_list_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        auto* child = row->get_child();
        if (!child) return;
        const auto name = child->get_tooltip_text(); // full path stored here
        if (!name.empty()) navigate_to(name);
    });
    dir_list_.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row || !row->get_child()) { selected_dir_.clear(); return; }
        selected_dir_ = row->get_child()->get_tooltip_text();
    });
    status_.add_css_class("muted");
    status_.set_margin_start(8);
    status_.set_halign(Gtk::Align::START);
    status_.set_visible(false);
    list_container_.append(dir_list_);
    list_container_.append(status_);
    scroller_.set_child(list_container_);

    // ── manual path entry ────────────────────────────────────────────
    path_entry_.set_hexpand(true);
    path_entry_.set_placeholder_text("/path/to/workspace");
    path_entry_.signal_activate().connect([this] { on_open(); });

    // ── footer ───────────────────────────────────────────────────────
    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* hidden_label = Gtk::make_managed<Gtk::Label>("Show hidden");
    show_hidden_.set_active(services_.folder_picker->get_boolean("show-hidden"));
    show_hidden_.property_active().signal_changed().connect([this] {
        services_.folder_picker->set_boolean("show-hidden",
                                             show_hidden_.get_active());
        rebuild_list();
    });
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    auto* open = Gtk::make_managed<Gtk::Button>("Open");
    open->add_css_class("suggested-action");
    open->signal_clicked().connect([this] { on_open(); });
    footer->append(*hidden_label);
    footer->append(show_hidden_);
    footer->append(*spacer);
    footer->append(*cancel);
    footer->append(*open);

    body->append(*header);
    body->append(breadcrumb_);
    body->append(scroller_);
    body->append(path_entry_);
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

    rebuild_list();
}

void WorkspacePickerDialog::set_status(const std::string& text) {
    // Keep the rows mounted for empty/error states so the ".." parent row
    // remains available (design §5.1).
    status_.set_text(text);
    status_.set_visible(!text.empty());
}

void WorkspacePickerDialog::navigate_to(const std::string& dir) {
    current_dir_ = dir;
    selected_dir_.clear();
    rebuild_list();
}

void WorkspacePickerDialog::rebuild_list() {
    breadcrumb_.set_text(current_dir_);
    path_entry_.set_text(current_dir_);

    // clear old rows
    while (auto* row = dir_list_.get_row_at_index(0))
        dir_list_.remove(*row);

    // ".." parent row
    const std::filesystem::path parent =
        std::filesystem::path(current_dir_).parent_path();
    if (!parent.empty() && parent.string() != current_dir_) {
        auto* row = folder_row("..");
        row->set_tooltip_text(parent.string());
        dir_list_.append(*row);
    }

    const bool show_hidden = show_hidden_.get_active();
    auto entries = FileService::browse_dirs(current_dir_, show_hidden);
    if (!entries) {
        set_status("Error: " + entries.error());
        return;
    }
    if (entries->empty()) {
        set_status("No subfolders");
        return;
    }
    set_status("");
    for (const auto& e : *entries) {
        auto* row = folder_row(e.name);
        row->set_tooltip_text(e.path); // full path for activation
        dir_list_.append(*row);
    }
}

void WorkspacePickerDialog::on_open() {
    // Prefer the typed path, then the selected row, then the breadcrumb (§5.1).
    std::string chosen = path_entry_.get_text();
    if (chosen.empty()) chosen = selected_dir_.empty() ? current_dir_ : selected_dir_;

    std::error_code ec;
    if (!std::filesystem::is_directory(chosen, ec)) {
        set_status("Error: not a directory: " + chosen);
        return;
    }
    signal_folder_chosen_.emit(std::filesystem::weakly_canonical(chosen, ec).string());
    hide();
}

void WorkspacePickerDialog::on_new_folder(const std::string& parent_dir,
                                          const std::string& name) {
    const std::string target = parent_dir + "/" + name;
    std::error_code ec;
    std::filesystem::create_directories(target, ec);
    if (ec) {
        set_status("Error creating folder: " + ec.message());
        return;
    }
    rebuild_list(); // refresh parent picker (§5.2)
}

} // namespace monkeybyte
