#pragma once

#include "monkeybyte/backend/FileService.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>
#include <vector>

namespace monkeybyte {

struct AppServices;

// §5.1 — custom workspace folder picker (stays within the filesystem; shows
// directories only). Emits the chosen absolute path on Open.
class WorkspacePickerDialog : public Gtk::Window {
public:
    WorkspacePickerDialog(Gtk::Window& parent, AppServices& services);

    sigc::signal<void(const std::string&)> signal_folder_chosen() {
        return signal_folder_chosen_;
    }

private:
    void navigate_to(const std::string& dir);
    void rebuild_list();
    void on_open();
    void on_new_folder(const std::string& parent_dir, const std::string& name);
    void set_status(const std::string& text); // muted loading/empty/error line

    AppServices& services_;
    std::string current_dir_;
    std::string selected_dir_; // explicitly selected row, if any

    Gtk::Label breadcrumb_;
    Gtk::ListBox dir_list_;
    Gtk::Label status_;
    Gtk::Box list_container_{Gtk::Orientation::VERTICAL, 4};
    Gtk::Entry path_entry_;
    Gtk::Switch show_hidden_;
    Gtk::ScrolledWindow scroller_;

    sigc::signal<void(const std::string&)> signal_folder_chosen_;
};

} // namespace monkeybyte
