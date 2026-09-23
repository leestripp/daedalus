#pragma once

#include <gtkmm.h>
#include <sigc++/sigc++.h>
#include <string>
#include <vector>

namespace monkeybyte {

struct AppServices;

// Plan.md P7 — "New Workspace…": create a folder + pick a project type in
// one step. Scaffolds the type's starter files and a stamped AGENTS.md,
// then emits the new path so callers open it like a picked workspace.
// Distinct from Open Workspace, which only picks an EXISTING folder.
class NewWorkspaceDialog : public Gtk::Window {
public:
    NewWorkspaceDialog(Gtk::Window& parent, AppServices& services);

    sigc::signal<void(const std::string&)> signal_workspace_created() {
        return signal_workspace_created_;
    }

private:
    void on_create();

    AppServices& services_;

    Gtk::Entry name_entry_;
    Gtk::Entry location_entry_;
    Gtk::DropDown type_dropdown_;
    Glib::RefPtr<Gtk::StringList> type_model_;
    std::vector<std::string> type_ids_; // parallel to type_model_
    Gtk::Label status_;

    sigc::signal<void(const std::string&)> signal_workspace_created_;
};

} // namespace monkeybyte
