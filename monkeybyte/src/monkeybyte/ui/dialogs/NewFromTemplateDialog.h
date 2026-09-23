#pragma once

#include "monkeybyte/backend/FileTemplates.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>
#include <vector>

namespace monkeybyte {

struct AppServices;

// §4.2 — "Add File from Template…": pick a starter template + filename,
// creates the file in the workspace root. Emits the created path like
// NewFileDialog so callers can wire refresh + open identically.
class NewFromTemplateDialog : public Gtk::Window {
public:
    NewFromTemplateDialog(Gtk::Window& parent, AppServices& services);

    sigc::signal<void(const std::string&)> signal_file_created() {
        return signal_file_created_;
    }

private:
    void on_row_selected(Gtk::ListBoxRow* row);
    void on_create();
    void set_status(const std::string& text);

    AppServices& services_;
    // Active project type's templates first, then the built-ins (Plan.md
    // P3). Row index == index into this vector.
    std::vector<FileTemplate> templates_;

    Gtk::ListBox template_list_;
    Gtk::Entry filename_entry_;
    Gtk::Label status_;
    Gtk::Button* create_btn_ = nullptr;

    sigc::signal<void(const std::string&)> signal_file_created_;
};

} // namespace monkeybyte
