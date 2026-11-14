#pragma once

#include <gtkmm.h>
#include <string>

namespace monkeybyte {

struct InterfaceSettings;

// Loads the dark/light Catppuccin CSS and swaps providers live (design §12.4).
// "system" follows the host desktop's color-scheme preference (design §11).
//
// Resolution order when theme == "system":
//   1. xdg-desktop-portal    `org.freedesktop.appearance` color-scheme
//                            (uint: 0 = none, 1 = prefer-dark, 2 = prefer-light)
//   2. GNOME                 `org.gnome.desktop.interface` color-scheme
//                            (enum: "default" | "prefer-dark" | "prefer-light")
//   3. fallback              "dark"
class ThemeManager {
public:
    ThemeManager(); // resolves data/theme/{dark,light}.css relative to the binary
    ~ThemeManager();

    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    void apply(const std::string& theme); // "system" | "dark" | "light"
    // The resolved theme actually in effect: "dark" | "light".
    const std::string& current() const { return current_; }

    // §4.5 Chat group — user appearance overrides layered above the theme
    // (provider priority APPLICATION+1). Only non-empty settings generate
    // rules; with nothing set the provider is detached and the theme shows
    // through unchanged. Idempotent; survives dark↔light switches.
    void set_chat_overrides(const InterfaceSettings& s);

    // Directory containing data/ (theme css, sourceview style schemes).
    static std::string data_dir();

    // Re-apply the current mode (insurance against early-startup settings
    // ordering: re-asserts the base-theme color scheme after the UI exists).
    void reapply() { apply(mode_); }

    // Emitted at the end of every apply() that resolved to dark or light;
    // widgets with theme-dependent non-CSS state (GtkSourceView scheme)
    // connect here to follow live switches.
    sigc::signal<void()> signal_applied() { return signal_applied_; }

private:
    // "system" → portal → GNOME interface → "dark".
    std::string resolve(const std::string& theme) const;

    // Point the base GTK theme at the requested scheme: gtk-interface-
    // color-scheme on GTK >= 4.20 (prefer-dark is deprecated there), with
    // gtk-application-prefer-dark-theme kept for older hosts.
    void set_color_scheme(Gtk::Settings* settings, bool dark);

    // GTK >= 4.20: the settings portal can overwrite the app-set color
    // scheme when it syncs. Keep re-asserting our resolved scheme (or
    // re-resolving, in "system" mode) whenever the setting changes.
    void on_scheme_notify();
    static void on_scheme_notify_cb(GtkSettings* s, GParamSpec*, gpointer self);

    Glib::RefPtr<Gtk::CssProvider> dark_;
    Glib::RefPtr<Gtk::CssProvider> light_;
    Glib::RefPtr<Gtk::CssProvider> chat_overrides_;
    bool chat_overrides_attached_ = false;
    // xdg-desktop-portal color-scheme (uint 0/1/2); null when the portal
    // schema is not installed (Flatpak-style installs).
    Glib::RefPtr<Gio::Settings> appearance_;
    // GNOME desktop color-scheme (enum: default|prefer-dark|prefer-light);
    // null on non-GNOME desktops that don't ship gnome-desktop schemas.
    Glib::RefPtr<Gio::Settings> gnome_interface_;
    sigc::signal<void()> signal_applied_;
    // 0 = unset, else GtkInterfaceColorScheme we last asserted (2/3).
    int desired_scheme_ = 0;
    std::string mode_ = "system";  // as requested via apply()
    std::string current_ = "dark"; // resolved
};

} // namespace monkeybyte
