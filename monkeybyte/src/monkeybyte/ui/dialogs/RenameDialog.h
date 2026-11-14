#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>

namespace monkeybyte {

// Small modal to rename a file or folder. Emits the new name; the caller
// performs the rename through FileService and refreshes.
class RenameDialog : public Gtk::Window {
public:
    RenameDialog(Gtk::Window& parent, const std::string& current_name,
                 bool is_dir);

    sigc::signal<void(const std::string&)> signal_renamed() {
        return signal_renamed_;
    }

private:
    void on_rename();

    Gtk::Entry name_entry_;
    sigc::signal<void(const std::string&)> signal_renamed_;
};

} // namespace monkeybyte
