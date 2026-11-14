#include "monkeybyte/ui/dialogs/ProviderEditDialog.h"

#include "monkeybyte/ui/UiGuard.h"

#include "monkeybyte/AppServices.h"

#include <daedalus/ProviderPlugin.h>
#include <daedalus/ProviderRegistry.h>
#include <daedalus/settings.h>

namespace monkeybyte {

UiProvider ProviderEditDialog::preset_minimax() {
    UiProvider p;
    p.name = "MiniMax";
    p.host = "https://api.minimax.io";
    p.plugin = "minimax";
    return p;
}
UiProvider ProviderEditDialog::preset_openai() {
    UiProvider p;
    p.name = "OpenAI";
    p.host = "https://api.openai.com";
    p.plugin = "openai-compatible";
    return p;
}
UiProvider ProviderEditDialog::preset_ollama() {
    UiProvider p;
    p.name = "Ollama";
    p.host = "http://localhost:11434";
    p.chat_endpoint = "/api/chat";
    // Pin the generic plugin explicitly: the lmstudio plugin's matches()
    // claims every loopback host, which would silently hijack Ollama.
    p.plugin = "openai-compatible";
    return p;
}
UiProvider ProviderEditDialog::preset_kimi() {
    UiProvider p;
    p.name = "Kimi";
    p.host = "https://api.kimi.com";
    // Kimi Code subscription keys are served under /coding/v1; the bare
    // host answers /v1/models but NOT the coding chat path (§4.5).
    p.chat_endpoint = "/coding/v1/chat/completions";
    p.discovery_endpoint = "/coding/v1/models";
    p.plugin = "moonshot";
    return p;
}
UiProvider ProviderEditDialog::preset_lmstudio() {
    UiProvider p;
    p.name = "LM Studio";
    p.host = "http://localhost:1234";
    // lmstudio plugin: requires_auth() == false — no API key needed (§4.5).
    p.plugin = "lmstudio";
    return p;
}

Gtk::Box* ProviderEditDialog::make_field(const Glib::ustring& label, Gtk::Widget& field) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->set_halign(Gtk::Align::START);
    l->add_css_class("dim-label");
    field.set_hexpand(true);
    row->append(*l);
    row->append(field);
    return row;
}

ProviderEditDialog::ProviderEditDialog(Gtk::Window& parent, AppServices& services,
                                       const std::optional<UiProvider>& existing)
    : services_(services) {
    set_title(existing ? "Edit Provider" : "Add Provider");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(420, -1);

    timeout_.set_range(5, 600);
    timeout_.set_increments(5, 30);

    // §ProviderPlugins — plugin choices: auto-detect + the daedalus
    // registry (built-ins self-register on first access).
    plugin_list_ = Gtk::StringList::create({});
    plugin_list_->append("Auto-detect");
    for (const auto& plugin : daedalus::ProviderRegistry::all())
        plugin_list_->append(plugin->name());
    plugin_.set_model(plugin_list_);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>(existing ? "Edit Provider" : "Add Provider");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close_btn);

    body->append(*header);
    body->append(*make_field("PROVIDER NAME", name_));
    body->append(*make_field("BASE URL", host_));
    body->append(*make_field("CHAT ENDPOINT", chat_endpoint_));
    body->append(*make_field("DISCOVERY ENDPOINT", discovery_endpoint_));
    body->append(*make_field("API KEY", api_key_));
    body->append(*make_field("TIMEOUT (SECONDS)", timeout_));
    body->append(*make_field("PLUGIN", plugin_));

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
        name_.set_editable(false); // name is the unique id (§5.3)
        host_.set_text(existing->host);
        chat_endpoint_.set_text(existing->chat_endpoint);
        discovery_endpoint_.set_text(existing->discovery_endpoint);
        api_key_.set_text(existing->api_key);
        timeout_.set_value(existing->timeout_seconds);
        if (!existing->plugin.empty()) {
            // index 0 is "Auto-detect"; unknown names fall back to it
            for (guint i = 1; i < plugin_list_->get_n_items(); ++i) {
                if (plugin_list_->get_string(i) == existing->plugin) {
                    plugin_.set_selected(i);
                    break;
                }
            }
        }
    } else {
        chat_endpoint_.set_text("/v1/chat/completions");
        discovery_endpoint_.set_text("/v1/models");
        timeout_.set_value(30);
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

void ProviderEditDialog::on_save() {
    if (name_.get_text().empty() || host_.get_text().empty()) return;
    UiProvider p;
    p.name = name_.get_text();
    p.host = host_.get_text();
    p.chat_endpoint = chat_endpoint_.get_text();
    p.discovery_endpoint = discovery_endpoint_.get_text();
    p.api_key = api_key_.get_text();
    p.timeout_seconds = timeout_.get_value_as_int();
    // index 0 = "Auto-detect" → stored as empty (daedalus auto-detects).
    if (const guint sel = plugin_.get_selected();
        sel > 0 && sel != GTK_INVALID_LIST_POSITION)
        p.plugin = plugin_list_->get_string(sel);
    // SQLite write can throw; keep the dialog open on failure.
    ui_guard("ProviderEditDialog::on_save", [&] {
        model_bridge::save_provider(daedalus::SettingsStore::instance(), p);
        signal_saved_.emit();
        hide();
    });
}

} // namespace monkeybyte
