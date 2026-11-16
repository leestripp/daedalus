#pragma once

#include "monkeybyte/backend/FileService.h"
#include "monkeybyte/backend/ModelBridge.h"
#include "monkeybyte/backend/ProjectTypes.h"
#include "monkeybyte/backend/SettingsStore.h"
#include "monkeybyte/backend/VoiceService.h"
#include "monkeybyte/ui/theme/ThemeManager.h"

#include <daedalus/settings.h>
#include <giomm/settings.h>
#include <optional>
#include <string>

namespace monkeybyte {

// Shared application services, owned by the Application/MainWindow and passed
// by reference to every panel.
struct AppServices {
    SettingsStore store;   // SQLite settings (authoritative)
    // Project type library (Plan.md): scans
    // $XDG_DATA_HOME/monkeybyte/project-types, seeding built-ins on
    // first run. Workspace type = AGENTS.md stamp / heuristics.
    ProjectTypeLibrary project_types;
    FileService files;     // workspace file service
    ThemeManager theme;
    // §CHIMPFLOW — one shared STT/TTS owner for Settings and Chat.
    std::shared_ptr<VoiceService> voice;

    // GSettings (UI cache only, design §10.2/§13)
    Glib::RefPtr<Gio::Settings> win;
    Glib::RefPtr<Gio::Settings> sidebar;
    Glib::RefPtr<Gio::Settings> chat_panel;
    Glib::RefPtr<Gio::Settings> folder_picker;
    Glib::RefPtr<Gio::Settings> editor;
    Glib::RefPtr<Gio::Settings> theme_settings;
    Glib::RefPtr<Gio::Settings> chat;

    AppServices()
        : voice(std::make_shared<VoiceService>(store)),
          win(Gio::Settings::create("io.github.monkeybyte.window")),
          sidebar(Gio::Settings::create("io.github.monkeybyte.sidebar")),
          chat_panel(Gio::Settings::create("io.github.monkeybyte.chat-panel")),
          folder_picker(Gio::Settings::create("io.github.monkeybyte.folder-picker")),
          editor(Gio::Settings::create("io.github.monkeybyte.editor")),
          theme_settings(Gio::Settings::create("io.github.monkeybyte.theme")),
          chat(Gio::Settings::create("io.github.monkeybyte.chat")) {
        voice->set_playback_volume(
            static_cast<float>(chat->get_double("playback-volume")));
        // One-time: move legacy monkeybyte.db model rows into the daedalus
        // provider/model registry (design §10.2). Idempotent.
        model_bridge::migrate_legacy_models(store, daedalus::SettingsStore::instance());
        if (auto ws = store.get("workspace"))
            files.set_workspace(*ws);
        files.set_show_hidden(store.load_interface().show_hidden);
        theme.apply(theme_settings->get_string("name"));
        theme.set_chat_overrides(store.load_interface()); // §4.5 chat appearance
    }
};

} // namespace monkeybyte
