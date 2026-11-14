#include "monkeybyte/ui/dialogs/McpJsonImportDialog.h"

#include "monkeybyte/ui/UiGuard.h"

#include "monkeybyte/backend/McpBridge.h"

#include <daedalus/settings.h>

namespace monkeybyte {

McpJsonImportDialog::McpJsonImportDialog(Gtk::Window& parent) {
    set_title("Add MCP Server from JSON");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(520, -1);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>("Add MCP Server from JSON");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close_btn);
    body->append(*header);

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Paste the JSON your MCP provider gives you — a full \"mcpServers\" "
        "block or a single server entry.");
    hint->set_halign(Gtk::Align::START);
    hint->set_wrap(true);
    body->append(*hint);

    auto* json_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    json_scroll->set_min_content_height(200);
    json_scroll->set_child(json_);
    json_scroll->set_hexpand(true);
    json_.set_monospace(true);
    json_.set_wrap_mode(Gtk::WrapMode::NONE);
    body->append(*json_scroll);

    error_.add_css_class("error");
    error_.set_halign(Gtk::Align::START);
    error_.set_wrap(true);
    error_.set_visible(false);
    body->append(error_);

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    auto* import = Gtk::make_managed<Gtk::Button>("Import");
    import->add_css_class("suggested-action");
    import->signal_clicked().connect([this] { on_import(); });
    footer->append(*cancel);
    footer->append(*import);
    body->append(*footer);

    set_child(*body);

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { hide(); return true; }
            return false;
        },
        false);
    add_controller(keys);
}

void McpJsonImportDialog::on_import() {
    const auto text = json_.get_buffer()->get_text();
    if (text.empty()) return;

    auto parsed = mcp_bridge::parse_json(text);
    if (!parsed) {
        error_.set_text(parsed.error());
        error_.set_visible(true);
        return; // §5.4: don't close on invalid JSON
    }

    // SQLite writes can throw; keep the dialog open on failure.
    ui_guard("McpJsonImportDialog::on_import", [&] {
        auto& store = daedalus::SettingsStore::instance();
        for (const auto& s : *parsed) store.upsert_mcp_server(s);
        signal_saved_.emit();
        hide();
    });
}

} // namespace monkeybyte
