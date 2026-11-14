#pragma once

#include "monkeybyte/backend/ModelBridge.h"

#include <gtkmm.h>
#include <optional>
#include <sigc++/sigc++.h>

namespace monkeybyte {

struct AppServices;

// §5.3 — create or edit a shared provider configuration (host / key /
// endpoints). Models pick a provider from Settings → Models.
class ProviderEditDialog : public Gtk::Window {
public:
    // existing == nullopt → create mode (empty or preset-filled form).
    ProviderEditDialog(Gtk::Window& parent, AppServices& services,
                       const std::optional<UiProvider>& existing = std::nullopt);

    // Quick-fill presets for the "+ MiniMax / + OpenAI / + Ollama / + Kimi /
    // + LM Studio" buttons (§4.5). Each pins its daedalus provider plugin
    // explicitly so host auto-detection can't surprise (§ProviderPlugins).
    static UiProvider preset_minimax();
    static UiProvider preset_openai();
    static UiProvider preset_ollama();
    static UiProvider preset_kimi();
    static UiProvider preset_lmstudio();

    sigc::signal<void()> signal_saved() { return signal_saved_; }

private:
    void on_save();
    Gtk::Box* make_field(const Glib::ustring& label, Gtk::Widget& field);

    AppServices& services_;

    Gtk::Entry name_, host_, chat_endpoint_, discovery_endpoint_;
    Gtk::PasswordEntry api_key_;
    Gtk::SpinButton timeout_;
    // §ProviderPlugins — "Auto-detect" (index 0, stored as empty) plus every
    // plugin registered in daedalus::ProviderRegistry.
    Gtk::DropDown plugin_;
    Glib::RefPtr<Gtk::StringList> plugin_list_;

    sigc::signal<void()> signal_saved_;
};

} // namespace monkeybyte
