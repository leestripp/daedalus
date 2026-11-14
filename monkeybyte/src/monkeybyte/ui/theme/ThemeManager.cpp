#include "monkeybyte/ui/theme/ThemeManager.h"

#include "monkeybyte/backend/SettingsStore.h"
#include "monkeybyte/ui/chat/Markdown.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace monkeybyte {

namespace {

Glib::RefPtr<Gtk::CssProvider> load_provider(const std::string& path) {
    auto provider = Gtk::CssProvider::create();
    provider->load_from_path(path);
    return provider;
}

// Probe a schema id without throwing on missing schemas.
bool schema_exists(const std::string& schema_id) {
    const auto src = Gio::SettingsSchemaSource::get_default();
    return src && src->lookup(schema_id, true);
}

} // namespace

std::string ThemeManager::data_dir() {
    // Resolve relative to the executable, not the CWD, so the app works
    // regardless of where it is launched from.
    std::filesystem::path exe_dir;
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        exe_dir = exe.parent_path();
    else
        exe_dir = Glib::get_current_dir();

    // build tree: data/ is copied next to the binary by CMake (POST_BUILD)
    if (Glib::file_test((exe_dir / "data/theme/dark.css").string(), Glib::FileTest::EXISTS))
        return (exe_dir / "data").string();
    // prefix install: <prefix>/bin/monkeybyte -> <prefix>/share/monkeybyte/data
    if (Glib::file_test((exe_dir / "../share/monkeybyte/data/theme/dark.css").string(),
                        Glib::FileTest::EXISTS))
        return (exe_dir / "../share/monkeybyte/data").string();
    // fallback: source tree (running from build/ subdir)
    if (Glib::file_test(exe_dir / "../data/theme/dark.css", Glib::FileTest::EXISTS))
        return (exe_dir / "../data").string();
    return (exe_dir / "data").string();
}

ThemeManager::ThemeManager() {
    const std::string dir = data_dir() + "/theme";
    dark_ = load_provider(dir + "/dark.css");
    light_ = load_provider(dir + "/light.css");

    // 1) xdg-desktop-portal color-scheme (GNOME 42+, KDE; absent on stock
    //    GNOME without xdg-desktop-portal-gnome running).
    if (schema_exists("org.freedesktop.appearance")) {
        appearance_ = Gio::Settings::create("org.freedesktop.appearance");
        appearance_->signal_changed("color-scheme").connect([this](const Glib::ustring&) {
            if (mode_ == "system") apply("system"); // live follow (§11)
        });
    }

    // 2) GNOME desktop color-scheme (always present on real GNOME installs).
    //    This is the schema the GNOME Settings → Appearance → Dark toggle
    //    actually writes to; reads `default` | `prefer-dark` | `prefer-light`.
    if (schema_exists("org.gnome.desktop.interface")) {
        gnome_interface_ = Gio::Settings::create("org.gnome.desktop.interface");
        gnome_interface_->signal_changed("color-scheme").connect([this](const Glib::ustring&) {
            if (mode_ == "system") apply("system"); // live follow (§11)
        });
    }
    // GTK >= 4.20 portal-sync guard (see on_scheme_notify).
#if GTK_CHECK_VERSION(4, 20, 0)
    if (auto settings = Gtk::Settings::get_default())
        g_signal_connect(settings->gobj(), "notify::gtk-interface-color-scheme",
                         G_CALLBACK(&ThemeManager::on_scheme_notify_cb), this);
#endif

}

ThemeManager::~ThemeManager() {
    apply("");
    if (chat_overrides_attached_) {
        if (auto display = Gdk::Display::get_default())
            Gtk::StyleContext::remove_provider_for_display(display, chat_overrides_);
        chat_overrides_attached_ = false;
    }
}

std::string ThemeManager::resolve(const std::string& theme) const {
    if (theme != "system") return theme;

    // (1) portal: 0 = no preference, 1 = prefer-dark, 2 = prefer-light.
    if (appearance_) {
        const unsigned scheme = appearance_->get_uint("color-scheme");
        if (scheme == 2) return "light";
        if (scheme == 1) return "dark";
        // 0 = no preference → fall through to GNOME/GTK below.
    }

    // (2) GNOME native interface. "default" usually defers to the user's
    //     chosen libadwaita/GTK theme, which itself inspects
    //     `gtk-application-prefer-dark`; we treat "default" as dark to
    //     match the GNOME dark-mode toggle intent.
    if (gnome_interface_) {
        const auto scheme = gnome_interface_->get_string("color-scheme");
        if (scheme == "prefer-light") return "light";
        if (scheme == "prefer-dark") return "dark";
        // "default" → fall through.
    }

    // (3) fallback — also matches GTK's own `gtk-application-prefer-dark`
    //     default, which is false on most desktops.
    return "dark";
}

void ThemeManager::on_scheme_notify_cb(GtkSettings*, GParamSpec*, gpointer self) {
    static_cast<ThemeManager*>(self)->on_scheme_notify();
}

void ThemeManager::on_scheme_notify() {
#if GTK_CHECK_VERSION(4, 20, 0)
    if (desired_scheme_ == 0) return;
    auto* s = gtk_settings_get_default();
    gint v = 0;
    g_object_get(s, "gtk-interface-color-scheme", &v, nullptr);
    if (v == desired_scheme_) return; // our value is in effect
    if (mode_ == "system")
        apply("system"); // portal preference changed: re-resolve + re-assert
    else
        g_object_set(s, "gtk-interface-color-scheme", desired_scheme_, nullptr);
#endif
}

void ThemeManager::set_color_scheme(Gtk::Settings* settings, bool dark) {
    // GTK >= 4.20 deprecated gtk-application-prefer-dark-theme in favour of
    // gtk-interface-color-scheme (GtkInterfaceColorScheme: DARK=2, LIGHT=3);
    // the base theme (Adwaita) renders per that setting, so without this the
    // chrome stays light while only our CSS-painted widgets go dark (design
    // section 11). prefer-dark is kept for GTK < 4.20 hosts.
#if GTK_CHECK_VERSION(4, 20, 0)
    desired_scheme_ = dark ? GTK_INTERFACE_COLOR_SCHEME_DARK
                           : GTK_INTERFACE_COLOR_SCHEME_LIGHT;
    g_object_set(settings->gobj(), "gtk-interface-color-scheme",
                 desired_scheme_, nullptr);
#endif
    settings->property_gtk_application_prefer_dark_theme() = dark;
}

void ThemeManager::apply(const std::string& theme) {
    mode_ = theme;
    const std::string resolved = resolve(theme);

    auto display = Gdk::Display::get_default();
    if (!display) return;

    auto remove = [&](const Glib::RefPtr<Gtk::CssProvider>& p) {
        Gtk::StyleContext::remove_provider_for_display(display, p);
    };
    auto add = [&](const Glib::RefPtr<Gtk::CssProvider>& p) {
        Gtk::StyleContext::add_provider_for_display(
            display, p, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    };

    auto* gtk_settings = Gtk::Settings::get_default().get();
    if (resolved == "light") {
        remove(dark_);
        add(light_);
        current_ = "light";
        set_color_scheme(gtk_settings, false);
    } else if (resolved == "dark") {
        remove(light_);
        add(dark_);
        current_ = "dark";
        set_color_scheme(gtk_settings, true);
    } else {
        remove(dark_);
        remove(light_);
    }

    // §7.4: markdown span colors are hardcoded Pango, not CSS, so they must
    // follow the resolved theme explicitly (empty apply() at shutdown keeps
    // the last palette — harmless).
    if (resolved == "light")
        Markdown::set_palette(Markdown::light_palette());
    else if (resolved == "dark")
        Markdown::set_palette(Markdown::dark_palette());
    if (resolved == "light" || resolved == "dark")
        signal_applied_.emit();
}

// §4.5 Chat group — generated override CSS layered one priority step above
// the theme. Only set values emit rules, so untouched settings keep the
// theme look; colors apply to both themes (Reset restores the theme).
void ThemeManager::set_chat_overrides(const InterfaceSettings& s) {
    std::string css;
    if (!s.chat_bg.empty())
        css += ".chat-panel { background-color: " + s.chat_bg + "; }\n";
    if (!s.chat_user_bg.empty())
        css += ".message-user { background-color: " + s.chat_user_bg + "; }\n";
    if (!s.chat_user_text.empty())
        css += ".message-user { color: " + s.chat_user_text + "; }\n";
    if (!s.chat_assistant_bg.empty())
        css += ".message-assistant { background-color: " + s.chat_assistant_bg + "; }\n";
    if (!s.chat_assistant_text.empty())
        css += ".message-assistant { color: " + s.chat_assistant_text + "; }\n";
    if (s.chat_font_size > 0)
        css += ".message-user, .message-assistant { font-size: " +
               std::to_string(s.chat_font_size) + "px; }\n";

    auto display = Gdk::Display::get_default();
    if (!display) return;

    if (css.empty()) {
        if (chat_overrides_attached_) {
            Gtk::StyleContext::remove_provider_for_display(display, chat_overrides_);
            chat_overrides_attached_ = false;
        }
        return;
    }
    if (!chat_overrides_)
        chat_overrides_ = Gtk::CssProvider::create();
    chat_overrides_->load_from_string(css);
    if (!chat_overrides_attached_) {
        Gtk::StyleContext::add_provider_for_display(
            display, chat_overrides_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
        chat_overrides_attached_ = true;
    }
}

} // namespace monkeybyte
