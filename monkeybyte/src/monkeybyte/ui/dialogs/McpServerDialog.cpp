#include "monkeybyte/ui/dialogs/McpServerDialog.h"

#include "monkeybyte/ui/UiGuard.h"

#include <daedalus/settings.h>
#include <nlohmann/json.hpp>

namespace monkeybyte {

McpServerDialog::McpServerDialog(Gtk::Window& parent,
                                 const std::optional<McpServer>& existing) {
    set_title(existing ? "Edit MCP Server" : "Add MCP Server");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(440, -1);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>(
        existing ? "Edit MCP Server" : "Add MCP Server");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close_btn);
    body->append(*header);

    auto field = [&](const Glib::ustring& label, Gtk::Widget& w) {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        auto* l = Gtk::make_managed<Gtk::Label>(label);
        l->set_halign(Gtk::Align::START);
        w.set_hexpand(true);
        row->append(*l);
        row->append(w);
        body->append(*row);
    };

    field("SERVER NAME", name_);
    field("COMMAND", command_);

    auto* env_label = Gtk::make_managed<Gtk::Label>("ENVIRONMENT (JSON)");
    env_label->set_halign(Gtk::Align::START);
    body->append(*env_label);
    auto* env_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    env_scroll->set_min_content_height(80);
    env_scroll->set_child(env_);
    env_scroll->set_hexpand(true);
    env_.get_buffer()->set_text("{ \"KEY\": \"value\" }");
    env_.set_monospace(true);
    body->append(*env_scroll);

    auto* enabled_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* enabled_label = Gtk::make_managed<Gtk::Label>("Enabled");
    enabled_label->set_hexpand(true);
    enabled_label->set_halign(Gtk::Align::START);
    enabled_.set_active(existing ? existing->enabled : true); // default on (§5.4)
    enabled_row->append(*enabled_label);
    enabled_row->append(enabled_);
    body->append(*enabled_row);

    error_.add_css_class("error");
    error_.set_halign(Gtk::Align::START);
    error_.set_visible(false);
    body->append(error_);

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
        name_.set_text(existing->name);
        command_.set_text(existing->command);
        env_.get_buffer()->set_text(existing->env_json.empty() ? "{}" : existing->env_json);
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

void McpServerDialog::on_save() {
    if (name_.get_text().empty()) return;
    const auto env_text = env_.get_buffer()->get_text();

    auto parsed = nlohmann::json::parse(env_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error_.set_text("Environment must be a valid JSON object.");
        error_.set_visible(true);
        return; // §5.4: don't close on invalid JSON
    }

    McpServer s;
    s.name = name_.get_text();
    s.command = command_.get_text();
    s.env_json = parsed.dump();
    s.enabled = enabled_.get_active();
    // SQLite write can throw; keep the dialog open on failure (ui_guard
    // logs and degrades to a no-op).
    ui_guard("McpServerDialog::on_save", [&] {
        mcp_bridge::save(daedalus::SettingsStore::instance(), s);
        signal_saved_.emit();
        hide();
    });
}

} // namespace monkeybyte
