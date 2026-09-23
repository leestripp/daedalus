#pragma once

#include "monkeybyte/backend/SettingsStore.h"

#include <gtkmm.h>
#include <optional>
#include <sigc++/sigc++.h>

namespace monkeybyte {

struct AppServices;

// §5.5 — create or edit a system prompt. Create mode pre-fills the body with
// the current default prompt so users can branch from it.
class SystemPromptDialog : public Gtk::Window {
public:
    SystemPromptDialog(Gtk::Window& parent, AppServices& services,
                       const std::optional<SystemPrompt>& existing = std::nullopt);

    sigc::signal<void()> signal_saved() { return signal_saved_; }

private:
    void on_save();

    AppServices& services_;
    long prompt_id_ = 0; // 0 = insert

    Gtk::Entry name_;
    Gtk::TextView prompt_;
    Gtk::CheckButton set_default_;

    sigc::signal<void()> signal_saved_;
};

} // namespace monkeybyte
