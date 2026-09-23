#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/ui/MainWindow.h"
#include "monkeybyte/ui/UiGuard.h"
#include "monkeybyte/ui/theme/ThemeManager.h"

#include <gtkmm.h>

#include <filesystem>
#include <locale>
#include <memory>

int main(int argc, char* argv[]) {
    // NON_UNIQUE: every process is its own primary (no D-Bus uniqueness), so
    // multiple monkeybyte instances can run side by side — one per workspace.
    // HANDLES_OPEN: `monkeybyte <dir>` is delivered to *this* process via
    // signal_open(), letting each instance start on its own workspace.
    auto app = Gtk::Application::create(
        "io.github.monkeybyte",
        Gio::Application::Flags::NON_UNIQUE | Gio::Application::Flags::HANDLES_OPEN);

    // glibmm init (inside Gtk::Application::create) calls
    // std::locale::global(std::locale("")), making every subsequent C++
    // stream locale-aware -- under e.g. en_AU.UTF-8, `os << 37519` yields
    // "37,519". OmniNet's HTTP request builder uses std::ostringstream, so
    // any request body >= 1000 bytes went out with `Content-Length:
    // 37,519`, which Cloudflare's edge rejects with a plain 400 HTML page
    // (the "cloudflare 400" bug). Restore the classic locale: wire formats
    // must be locale-independent. (GTK itself uses the C locale via
    // setlocale and is unaffected.)
    std::locale::global(std::locale::classic());

    // Taskbar / alt-tab icon; resolves to resources/monkeybyte.svg once
    // installed into the hicolor theme (see CMake install rules).
    Gtk::Window::set_default_icon_name("monkeybyte");

    // App-private symbolic icons (design §3): GTK recolors *-symbolic SVGs
    // to the widget's CSS foreground color, so they follow the active theme.
    // Registered before any window exists — tree rows, editor tabs, tool
    // cards and the activity bar all resolve icons from here.
    if (auto display = Gdk::Display::get_default())
        Gtk::IconTheme::get_for_display(display)->add_search_path(
            monkeybyte::ThemeManager::data_dir() + "/icons");

    // One AppServices per process; created lazily on first activate/open.
    // (A non-unique app never shares services across processes — each
    // instance opens its own SQLite/GSettings handles; WAL + busy_timeout in
    // SettingsStore and dconf's last-writer-wins keep that race-safe.)
    std::unique_ptr<monkeybyte::AppServices> services;

    const auto open_window = [&app, &services](const std::string& workspace_hint) {
        // AppServices construction opens both SQLite DBs and the GSettings
        // schemas — an exception escaping activate would abort at startup.
        monkeybyte::ui_guard("app activate", [&] {
            if (!services)
                services = std::make_unique<monkeybyte::AppServices>();
            if (!workspace_hint.empty()) {
                // Seed this instance's workspace before the window is built;
                // persist exactly like the folder picker does
                // (MainWindow::on_workspace_changed).
                services->store.set("workspace", workspace_hint);
                services->files.set_workspace(workspace_hint);
            }
            auto* window = Gtk::make_managed<monkeybyte::MainWindow>(*services);
            app->add_window(*window);
            window->present();
        });
    };

    app->signal_activate().connect([&open_window] { open_window({}); });

    app->signal_open().connect(
        [&open_window](const Gio::Application::type_vec_files& files,
                       const Glib::ustring&) {
            // First folder argument wins; a file argument opens its parent
            // folder. Extra arguments are ignored (one window per launch).
            std::string hint;
            if (!files.empty()) {
                namespace fs = std::filesystem;
                const fs::path p = files.front()->get_path();
                std::error_code ec;
                hint = fs::is_directory(p, ec) ? p.string()
                                               : p.parent_path().string();
            }
            open_window(hint);
        });

    return app->run(argc, argv);
}
