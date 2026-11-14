#include "monkeybyte/ui/dialogs/NewFromTemplateDialog.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/FileTemplates.h"
#include "monkeybyte/backend/ProjectTypes.h"

#include <fstream>
#include <sstream>

#include <filesystem>

namespace monkeybyte {

NewFromTemplateDialog::NewFromTemplateDialog(Gtk::Window& parent,
                                             AppServices& services)
    : services_(services) {
    set_title("New File from Template");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(380, -1);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>("New File from Template");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close = Gtk::make_managed<Gtk::Button>("×");
    close->set_has_frame(false);
    close->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close);

    // Template list — one row per FileTemplate, index == row index.
    // Plan.md P3: the active project type's <id>.templates/ files come
    // first (labelled with the type name), then the built-ins.
    if (const auto& ws = services_.files.workspace(); !ws.empty()) {
        if (const auto* type = services_.project_types.detect(ws.string())) {
            std::error_code ec;
            for (const auto& entry :
                 std::filesystem::directory_iterator(type->templates_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const std::string path = entry.path().string();
                const std::string fname = entry.path().filename().string();
                templates_.push_back(FileTemplate{
                    fname + " (" + type->name + ")",
                    fname,
                    [path](const std::string&) {
                        std::ifstream in(path);
                        std::ostringstream ss;
                        ss << in.rdbuf();
                        return ss.str();
                    }});
            }
        }
    }
    const auto& builtins = file_templates();
    templates_.insert(templates_.end(), builtins.begin(), builtins.end());

    for (const auto& t : templates_) {
        auto* label = Gtk::make_managed<Gtk::Label>(t.label);
        label->set_halign(Gtk::Align::START);
        label->set_margin(4);
        template_list_.append(*label);
    }
    template_list_.set_selection_mode(Gtk::SelectionMode::SINGLE);
    template_list_.add_css_class("navigation-sidebar");
    template_list_.signal_row_selected().connect(
        [this](Gtk::ListBoxRow* row) { on_row_selected(row); });
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_child(template_list_);
    scroller->set_size_request(-1, 160);
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);

    auto* name_label = Gtk::make_managed<Gtk::Label>("File name:");
    name_label->set_halign(Gtk::Align::START);
    filename_entry_.set_hexpand(true);
    filename_entry_.signal_changed().connect([this] {
        if (create_btn_)
            create_btn_->set_sensitive(!filename_entry_.get_text().empty());
    });
    filename_entry_.signal_activate().connect([this] { on_create(); });

    auto* dest = Gtk::make_managed<Gtk::Label>(
        "Will be created in: " + services_.files.workspace().string());
    dest->set_halign(Gtk::Align::START);
    dest->add_css_class("muted");

    status_.set_halign(Gtk::Align::START);
    status_.add_css_class("error-label");

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    create_btn_ = Gtk::make_managed<Gtk::Button>("Create");
    create_btn_->add_css_class("suggested-action");
    create_btn_->signal_clicked().connect([this] { on_create(); });
    footer->append(*cancel);
    footer->append(*create_btn_);

    body->append(*header);
    body->append(*scroller);
    body->append(*name_label);
    body->append(filename_entry_);
    body->append(*dest);
    body->append(status_);
    body->append(*footer);
    set_child(*body);

    if (auto* first = template_list_.get_row_at_index(0))
        template_list_.select_row(*first);

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

void NewFromTemplateDialog::on_row_selected(Gtk::ListBoxRow* row) {
    if (!row) return;
    const auto& t = templates_.at(row->get_index());
    filename_entry_.set_text(t.default_name);
    // Pre-select the basename so retyping replaces it, keeping the extension.
    const auto dot = t.default_name.find_last_of('.');
    filename_entry_.select_region(
        0, dot == std::string::npos || dot == 0
               ? -1 : static_cast<int>(dot));
    set_status("");
}

void NewFromTemplateDialog::on_create() {
    auto* row = template_list_.get_selected_row();
    const auto name = std::string(filename_entry_.get_text());
    if (!row || name.empty()) return;
    if (name.find('/') != std::string::npos || name == "." || name == "..") {
        set_status("Name must be a plain file name (no path).");
        return;
    }
    const auto& t = templates_.at(row->get_index());
    const auto target =
        (services_.files.workspace() / name).string();
    if (auto r = services_.files.write(target, t.make_content(name))) {
        signal_file_created_.emit(target);
        hide();
    } else {
        set_status(r.error());
    }
}

void NewFromTemplateDialog::set_status(const std::string& text) {
    status_.set_text(text);
}

} // namespace monkeybyte
