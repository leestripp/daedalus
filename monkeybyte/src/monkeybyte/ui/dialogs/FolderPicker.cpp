#include "monkeybyte/ui/dialogs/FolderPicker.h"

#include "monkeybyte/backend/FileService.h"

#include <filesystem>

namespace monkeybyte {

namespace fs = std::filesystem;

namespace {

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

// Canonicalize the confinement root; error_code overload so a bad path
// never throws out of a GTK callback.
std::string canonical_root(const std::string& root) {
    if (root.empty()) return {};
    std::error_code ec;
    auto s = fs::weakly_canonical(root, ec).string();
    return ec ? std::string() : s;
}

} // namespace

FolderPicker::FolderPicker(const std::string& root,
                           const std::string& initial_dir)
    : Gtk::Box(Gtk::Orientation::VERTICAL, 8), root_(canonical_root(root)) {
    current_dir_ = root_;
    if (!initial_dir.empty() && within_root(initial_dir)) {
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(initial_dir, ec).string();
        if (!ec) current_dir_ = canonical;
    }

    // ── breadcrumb / current folder ────────────────────────────────────
    breadcrumb_.set_halign(Gtk::Align::START);
    breadcrumb_.set_ellipsize(Pango::EllipsizeMode::START);
    breadcrumb_.add_css_class("monospace");

    // ── directory list ─────────────────────────────────────────────────
    scroller_.set_vexpand(true);
    scroller_.set_min_content_height(200);
    dir_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    dir_list_.signal_row_activated().connect([this](Gtk::ListBoxRow* row) {
        auto* child = row->get_child();
        if (!child) return;
        const auto path = child->get_tooltip_text();
        if (!path.empty()) navigate_to(path);
    });
    dir_list_.signal_row_selected().connect([this](Gtk::ListBoxRow* row) {
        if (!row || !row->get_child()) { selected_dir_.clear(); return; }
        selected_dir_ = row->get_child()->get_tooltip_text();
    });
    status_.add_css_class("muted");
    status_.set_margin_start(8);
    status_.set_halign(Gtk::Align::START);
    status_.set_visible(false);
    // Rows + status live together in the scroller; set_status() only toggles
    // the label, so the ".." row never disappears (design §5.1).
    auto* list_container = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    list_container->append(dir_list_);
    list_container->append(status_);
    scroller_.set_child(*list_container);

    // ── manual path entry ──────────────────────────────────────────────
    path_entry_.set_hexpand(true);
    path_entry_.set_placeholder_text("/path/to/folder");

    append(breadcrumb_);
    append(scroller_);
    append(path_entry_);

    rebuild_list();
}

std::string FolderPicker::chosen_dir() const {
    std::string folder = path_entry_.get_text();
    if (folder.empty())
        folder = selected_dir_.empty() ? current_dir_ : selected_dir_;
    return folder;
}

void FolderPicker::set_status(const std::string& text) {
    status_.set_text(text);
    status_.set_visible(!text.empty());
}

bool FolderPicker::within_root(const std::string& path) const {
    if (root_.empty()) return false;
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec).string();
    if (ec) return false;
    return canonical == root_ || canonical.starts_with(root_ + "/");
}

void FolderPicker::navigate_to(const std::string& dir) {
    if (!within_root(dir)) return;
    current_dir_ = dir;
    selected_dir_.clear();
    rebuild_list();
}

void FolderPicker::rebuild_list() {
    breadcrumb_.set_text(current_dir_);
    path_entry_.set_text(current_dir_);

    while (auto* row = dir_list_.get_row_at_index(0))
        dir_list_.remove(*row);

    if (root_.empty()) {
        set_status("No workspace selected.");
        return;
    }

    // ".." row, but never above the root.
    const fs::path parent = fs::path(current_dir_).parent_path();
    if (!parent.empty() && within_root(parent.string()) &&
        parent.string() != current_dir_) {
        auto* row = folder_row("..");
        row->set_tooltip_text(parent.string());
        dir_list_.append(*row);
    }

    auto entries = FileService::browse_dirs(current_dir_, false);
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
        row->set_tooltip_text(e.path);
        dir_list_.append(*row);
    }
}

} // namespace monkeybyte
