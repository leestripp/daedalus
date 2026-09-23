#include "monkeybyte/ui/dialogs/SystemPromptDialog.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/ui/UiGuard.h"

namespace monkeybyte {

SystemPromptDialog::SystemPromptDialog(Gtk::Window& parent, AppServices& services,
                                       const std::optional<SystemPrompt>& existing)
    : services_(services) {
    set_title(existing ? "Edit System Prompt" : "New System Prompt");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(480, 420);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>(
        existing ? "Edit System Prompt" : "New System Prompt");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close_btn);
    body->append(*header);

    auto* name_label = Gtk::make_managed<Gtk::Label>("PROMPT NAME");
    name_label->set_halign(Gtk::Align::START);
    body->append(*name_label);
    name_.set_hexpand(true);
    body->append(name_);

    auto* prompt_label = Gtk::make_managed<Gtk::Label>("SYSTEM PROMPT");
    prompt_label->set_halign(Gtk::Align::START);
    body->append(*prompt_label);
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_vexpand(true);
    scroll->set_child(prompt_);
    prompt_.set_monospace(true);
    prompt_.set_wrap_mode(Gtk::WrapMode::WORD);
    body->append(*scroll);

    set_default_.set_label("Set as default prompt");
    set_default_.set_halign(Gtk::Align::START);
    body->append(set_default_);

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->add_css_class("suggested-action");
    save->signal_clicked().connect([this] { on_save(); });
    footer->append(*cancel);
    footer->append(*save);
    body->append(*footer);

    set_child(*body);

    if (existing) {
        prompt_id_ = existing->id;
        name_.set_text(existing->name);
        prompt_.get_buffer()->set_text(existing->prompt);
        set_default_.set_active(existing->is_default);
    } else {
        // Pre-fill with the current default prompt body (§5.5).
        std::vector<SystemPrompt> prompts;
        ui_guard("SystemPromptDialog",
                 [&] { prompts = services_.store.list_system_prompts(); });
        const auto def = std::find_if(prompts.begin(), prompts.end(),
                                      [](const SystemPrompt& p) { return p.is_default; });
        if (def != prompts.end()) prompt_.get_buffer()->set_text(def->prompt);
    }

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { hide(); return true; }
            return false;
        },
        false);
    add_controller(keys);
}

void SystemPromptDialog::on_save() {
    if (name_.get_text().empty()) return;
    SystemPrompt p;
    p.id = prompt_id_;
    p.name = name_.get_text();
    p.prompt = prompt_.get_buffer()->get_text();
    p.is_default = set_default_.get_active();
    bool saved = false;
    ui_guard("save_system_prompt", [&] {
        services_.store.save_system_prompt(p);
        saved = true;
    });
    if (!saved) return; // keep the dialog open so the text isn't lost
    signal_saved_.emit();
    hide();
}

} // namespace monkeybyte
