#pragma once

#include "monkeybyte/ui/dialogs/FolderPicker.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>

namespace monkeybyte {

// §4.2/§5.2 — folder selector + name entry for creating a folder. Emits the
// chosen parent dir + folder name; the caller performs the mkdir and
// refreshes. Used by the Workspace panel ('+' menu / tree context menu,
// rooted at the workspace) and the Workspace Picker (rooted at the dir being
// browsed).
class NewFolderDialog : public Gtk::Window {
public:
    NewFolderDialog(Gtk::Window& parent, const std::string& root,
                    const std::string& initial_dir = "");

    sigc::signal<void(const std::string&, const std::string&)>
    signal_folder_named() {
        return signal_folder_named_;
    }

private:
    void on_create();

    FolderPicker picker_;
    Gtk::Entry name_entry_;
    Gtk::Button* create_btn_ = nullptr;
    sigc::signal<void(const std::string&, const std::string&)>
        signal_folder_named_;
};

} // namespace monkeybyte
