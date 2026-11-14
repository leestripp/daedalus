#pragma once

#include "monkeybyte/tree/WorkspaceTree.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>

#include <string>
#include <vector>

namespace monkeybyte {

struct AppServices;

// §4.2 — Explorer: header actions, workspace path bar, lazy file tree.
class WorkspacePanel : public Gtk::Box {
public:
    explicit WorkspacePanel(AppServices& services);

    void refresh();
    void reveal(const std::string& path); // expand parents + select in tree

    sigc::signal<void(const std::string&, int)> signal_open_file() {
        return signal_open_file_;
    }
    // Tree context-menu "Open as …" (§6 preview tabs).
    sigc::signal<void(const std::string&, FileOpenMode)> signal_open_file_as() {
        return signal_open_file_as_;
    }
    sigc::signal<void(const std::string&)> signal_workspace_changed() {
        return signal_workspace_changed_;
    }
    // '+' menu "Set Workspace Type…" applied (Plan.md P6): the AGENTS.md
    // stamp is rewritten — the chat agent should rebuild so the new type's
    // system prompt applies.
    sigc::signal<void()> signal_project_type_changed() {
        return signal_project_type_changed_;
    }
    // Emitted after a tree context-menu delete/rename so open editor tabs can
    // be closed or retargeted.
    sigc::signal<void(const std::string&)> signal_path_deleted() {
        return signal_path_deleted_;
    }
    sigc::signal<void(const std::string&, const std::string&)> signal_path_renamed() {
        return signal_path_renamed_;
    }

private:
    void show_picker();
    void new_file(const std::string& initial_dir = "");
    void new_folder(const std::string& dir);
    void new_file_from_template();   // §4.2 '+' menu: template picker at root
    void import_files();             // §4.2 '+' menu: copy external files in
    void reveal_in_file_manager();   // §4.2 '+' menu: open root in OS browser
    void export_fountain_pdf(const std::string& path); // Plan.md P4
    void set_workspace_type();                     // Plan.md P6 '+' menu
    void open_terminal_here();       // §4.2 '+' menu: external terminal at root
    void rename_entry(const std::string& path, bool is_dir);
    void delete_entry(const std::string& path, bool is_dir);

    AppServices& services_;
    Gtk::Label path_bar_;
    WorkspaceTree tree_;


    sigc::signal<void(const std::string&, int)> signal_open_file_;
    sigc::signal<void(const std::string&, FileOpenMode)> signal_open_file_as_;
    sigc::signal<void(const std::string&)> signal_workspace_changed_;
    sigc::signal<void()> signal_project_type_changed_;
    sigc::signal<void(const std::string&)> signal_path_deleted_;
    sigc::signal<void(const std::string&, const std::string&)> signal_path_renamed_;
};

} // namespace monkeybyte
