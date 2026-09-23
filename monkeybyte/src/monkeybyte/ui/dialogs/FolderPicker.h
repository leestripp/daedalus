#pragma once

#include <gtkmm.h>
#include <string>

namespace monkeybyte {

// §4.2/§5.1 — folder selector shared by the New File / New Folder dialogs:
// breadcrumb, directory list (".." + subfolders) and a manual path entry.
// Navigation is confined to `root` (the workspace root for the Workspace
// panel dialogs; the picker dialog's current dir when launched from the
// Workspace Picker) — the list never goes above it.
class FolderPicker : public Gtk::Box {
public:
    // `root` must be absolute; an empty root renders a "No workspace
    // selected" status and picks nothing. `initial_dir` pre-selects a folder
    // inside root (ignored otherwise).
    explicit FolderPicker(const std::string& root, const std::string& initial_dir = "");

    // The folder to create in: the path entry when edited, else the selected
    // row, else the current directory.
    std::string chosen_dir() const;

    bool root_empty() const { return root_.empty(); }
    bool within_root(const std::string& path) const;
    // Shows `text` as a status line below the list ("" hides it). The rows
    // stay mounted so the ".." parent row remains available even in
    // empty/error states (same rule as the Workspace Picker, design §5.1).
    void set_status(const std::string& text);

private:
    void navigate_to(const std::string& dir);
    void rebuild_list();

    std::string root_;
    std::string current_dir_;
    std::string selected_dir_;

    Gtk::Label breadcrumb_;
    Gtk::ListBox dir_list_;
    Gtk::Label status_;
    Gtk::Entry path_entry_;
    Gtk::ScrolledWindow scroller_;
};

} // namespace monkeybyte
