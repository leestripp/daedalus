#pragma once

#include "monkeybyte/FileOpenMode.h"
#include "monkeybyte/backend/FileService.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>
#include <unordered_set>

namespace monkeybyte {

// §4.2 — lazy-loaded workspace file tree widget (ListBox-based; folders load
// on first expand, expansion state survives refresh).
class WorkspaceTree : public Gtk::ScrolledWindow {
public:
    WorkspaceTree();

    void set_workspace(FileService* files); // clears expansion, refreshes
    void refresh();                          // rebuild from root, keep expansion
    void select(const std::string& path);    // highlight + scroll to
    void refresh_folder(const std::string& path); // refresh one open folder
    void reveal_path(const std::string& path);    // expand parents, then select

    sigc::signal<void(const std::string&, int)> signal_open_file() {
        return signal_open_file_;
    }
    // Context-menu "Open as …" (§6 preview tabs): open in a viewer tab.
    sigc::signal<void(const std::string&, FileOpenMode)> signal_open_file_as() {
        return signal_open_file_as_;
    }
    sigc::signal<void()> signal_show_picker() { return signal_show_picker_; }
    sigc::signal<void()> signal_refreshed() { return signal_refreshed_; }

    // Context-menu actions (§4.2). The panel owning this tree performs the
    // actual work (dialogs + FileService) and calls refresh() afterwards.
    sigc::signal<void(const std::string&)> signal_new_file_in() {
        return signal_new_file_in_;
    }
    sigc::signal<void(const std::string&)> signal_new_folder_in() {
        return signal_new_folder_in_;
    }
    sigc::signal<void(const std::string&, bool)> signal_rename_requested() {
        return signal_rename_requested_;
    }
    // Plan.md P4 — tree context "Export to PDF…" on .fountain files.
    sigc::signal<void(const std::string&)> signal_export_pdf_requested() {
        return signal_export_pdf_requested_;
    }
    sigc::signal<void(const std::string&, bool)> signal_delete_requested() {
        return signal_delete_requested_;
    }

    // Logic exposed for unit tests (no GTK needed).
    static bool is_expanded(const std::unordered_set<std::string>& set,
                            const std::string& path);

private:
    struct Row {
        std::string path;
        bool is_dir;
        int depth;
        Gtk::ListBoxRow* widget;
    };

    void rebuild_rows();
    void toggle_folder(const std::string& path);
    // Click/context "Open": text-like files open internally, everything else
    // goes to the system-default application.
    void open_entry(const std::string& path);
    void open_external(const std::string& path);
    void set_status(const Glib::ustring& text); // muted empty/loading/error line
    void show_context_menu(Gtk::ListBoxRow& row, const std::string& path,
                           bool is_dir, double x, double y);

    FileService* files_ = nullptr;
    Gtk::ListBox list_;
    Gtk::Label status_;

    std::unordered_set<std::string> expanded_paths_; // §4.2 expandedPaths
    std::string selected_path_;

    sigc::signal<void(const std::string&, int)> signal_open_file_;
    sigc::signal<void(const std::string&, FileOpenMode)> signal_open_file_as_;
    sigc::signal<void()> signal_show_picker_;
    sigc::signal<void()> signal_refreshed_;
    sigc::signal<void(const std::string&)> signal_new_file_in_;
    sigc::signal<void(const std::string&)> signal_new_folder_in_;
    sigc::signal<void(const std::string&, bool)> signal_rename_requested_;
    sigc::signal<void(const std::string&, bool)> signal_delete_requested_;
    sigc::signal<void(const std::string&)> signal_export_pdf_requested_;
};

} // namespace monkeybyte
