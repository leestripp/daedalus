#include "monkeybyte/ui/dialogs/NewWorkspaceDialog.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/ProjectTypes.h"
#include "monkeybyte/ui/dialogs/WorkspacePickerDialog.h"

#include <cstdlib>
#include <filesystem>

namespace monkeybyte {

NewWorkspaceDialog::NewWorkspaceDialog(Gtk::Window& parent,
                                       AppServices& services)
    : services_(services) {
    set_title("New Workspace");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(420, -1);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(16);

    auto* title = Gtk::make_managed<Gtk::Label>("New Workspace");
    title->set_halign(Gtk::Align::START);
    body->append(*title);

    auto* blurb = Gtk::make_managed<Gtk::Label>(
        "Creates a new folder, scaffolds the project type's starter files "
        "and a stamped AGENTS.md, then opens it as the workspace.");
    blurb->set_wrap(true);
    blurb->set_halign(Gtk::Align::START);
    blurb->add_css_class("dim-label");
    body->append(*blurb);

    // ── name ──
    auto* name_label = Gtk::make_managed<Gtk::Label>("Name:");
    name_label->set_halign(Gtk::Align::START);
    body->append(*name_label);
    name_entry_.set_placeholder_text("my-project");
    name_entry_.signal_activate().connect([this] { on_create(); });
    body->append(name_entry_);

    // ── location (parent dir) + browse ──
    auto* loc_label = Gtk::make_managed<Gtk::Label>("Location:");
    loc_label->set_halign(Gtk::Align::START);
    body->append(*loc_label);
    auto* loc_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    location_entry_.set_hexpand(true);
    const char* home = std::getenv("HOME");
    location_entry_.set_text(home ? home : "/");
    auto* browse = Gtk::make_managed<Gtk::Button>("Browse…");
    browse->signal_clicked().connect([this] {
        // The folder picker is dirs-only — reuse it to choose the parent.
        auto* picker = Gtk::make_managed<WorkspacePickerDialog>(*this, services_);
        picker->signal_folder_chosen().connect(
            [this](const std::string& dir) { location_entry_.set_text(dir); });
        picker->present();
    });
    loc_row->append(location_entry_);
    loc_row->append(*browse);
    body->append(*loc_row);

    // ── project type ──
    auto* type_label = Gtk::make_managed<Gtk::Label>("Project Type:");
    type_label->set_halign(Gtk::Align::START);
    body->append(*type_label);
    type_model_ = Gtk::StringList::create();
    for (const auto& t : services_.project_types.list()) {
        type_model_->append(t.name + " — " + t.description);
        type_ids_.push_back(t.id);
    }
    type_dropdown_.set_model(type_model_);
    type_dropdown_.set_tooltip_text(
        "Focuses the agent (system prompt, templates) and scaffolds the "
        "type's starter files. Manage types in Settings → Workspace.");
    body->append(type_dropdown_);

    status_.set_halign(Gtk::Align::START);
    status_.add_css_class("error-label");
    body->append(status_);

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    auto* create = Gtk::make_managed<Gtk::Button>("Create && Open");
    create->add_css_class("suggested-action");
    create->signal_clicked().connect([this] { on_create(); });
    footer->append(*cancel);
    footer->append(*create);
    body->append(*footer);

    set_child(*body);

    // Escape closes (§5).
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { hide(); return true; }
            return false;
        },
        false);
    add_controller(keys);
}

void NewWorkspaceDialog::on_create() {
    const std::string name = name_entry_.get_text();
    const std::string location = location_entry_.get_text();
    if (name.empty() || name.find('/') != std::string::npos ||
        name == "." || name == "..") {
        status_.set_text("Name must be a plain folder name (no path).");
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(location, ec)) {
        status_.set_text("Location is not a directory: " + location);
        return;
    }
    const std::string target =
        (std::filesystem::path(location) / name).string();
    if (std::filesystem::exists(target, ec)) {
        status_.set_text("Already exists: " + target);
        return;
    }

    const ProjectType* type = nullptr;
    const auto sel = type_dropdown_.get_selected();
    if (sel != GTK_INVALID_LIST_POSITION && sel < type_ids_.size())
        type = services_.project_types.find(type_ids_[sel]);

    std::string err;
    if (!ProjectTypeLibrary::scaffold_workspace(target, type, &err)) {
        status_.set_text(err);
        return;
    }
    signal_workspace_created_.emit(target);
    hide();
}

} // namespace monkeybyte
