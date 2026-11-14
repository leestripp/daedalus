#pragma once

#include "monkeybyte/backend/FileService.h"
#include "monkeybyte/ui/dialogs/FolderPicker.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>

namespace monkeybyte {

struct AppServices;

// Folder selector + filename entry for creating a new file inside the
// current workspace (design §4.2, §5.1).
class NewFileDialog : public Gtk::Window {
public:
    // `initial_dir` pre-selects the folder the file is created in (must be
    // inside the workspace; ignored otherwise).
    NewFileDialog(Gtk::Window& parent, AppServices& services,
                  const std::string& initial_dir = "");

    sigc::signal<void(const std::string&)> signal_file_created() {
        return signal_file_created_;
    }

private:
    void on_create();

    AppServices& services_;
    FolderPicker picker_;

    Gtk::Entry filename_entry_;
    Gtk::Button* create_btn_ = nullptr;

    sigc::signal<void(const std::string&)> signal_file_created_;
};

} // namespace monkeybyte
