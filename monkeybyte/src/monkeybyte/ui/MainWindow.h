#pragma once

#include "monkeybyte/ui/ActivityBar.h"
#include "monkeybyte/http/ControlServer.h"

#include <gtkmm.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>

namespace monkeybyte {

struct AppServices;
class Sidebar;
class EditorArea;
class ChatPanel;
class StatusBar;

// Single top-level window (design §2). Owns all panels as direct children.
class MainWindow : public Gtk::ApplicationWindow {
public:
    explicit MainWindow(AppServices& services);
    ~MainWindow() override;

private:
    void size_allocate_vfunc(int width, int height, int baseline) override;
    void build_layout();
    void wire_signals();
    void wire_shortcuts();
    void on_panel_selected(ActivityBar::Panel panel);
    void on_workspace_changed(const std::string& path);
    void update_status_branch();
    void poll_git_status(); // §8 — timer tick: off-thread git refresh
    void update_title();
    bool on_close_request();
    std::pair<int, int> clamped_window_size() const;

    // Geometry persistence (§10.2): SQLite kv is authoritative; GSettings is
    // written too, but only as a cache — dconf is not guaranteed to persist
    // (e.g. containers sharing a foreign session bus), so restores must not
    // depend on it.
    int load_px(const char* key, const Glib::RefPtr<Gio::Settings>& cache,
                const char* cache_key) const;
    void save_px(const char* key, int value,
                 const Glib::RefPtr<Gio::Settings>& cache, const char* cache_key);

    // GNOME text-scaling-factor aware panel bounds (§2 resize grips).
    static double text_scale_factor();
    int sidebar_min() const;
    int sidebar_max() const;
    int chat_min() const;

    AppServices& services_;
    ControlServer control_server_;

    ActivityBar* activity_bar_ = nullptr;
    Sidebar* sidebar_ = nullptr;
    EditorArea* editor_area_ = nullptr;
    ChatPanel* chat_panel_ = nullptr;
    StatusBar* status_bar_ = nullptr;
    Gtk::Paned* outer_paned_ = nullptr; // [activity+sidebar] | inner
    Gtk::Paned* inner_paned_ = nullptr; // [editor] | chat

    // §8 — the status-bar git label is POLLED: branch switches, commits,
    // and working-tree edits happen outside the app's own signals, so a
    // timer re-runs GitService::status off the UI thread and marshals the
    // text back via signal_idle. The shared atomics let the detached
    // worker bail out safely after the window is destroyed.
    sigc::connection git_poll_conn_;
    std::shared_ptr<std::atomic<bool>> alive_ =
        std::make_shared<std::atomic<bool>>(true);
    std::shared_ptr<std::atomic<bool>> git_poll_in_flight_ =
        std::make_shared<std::atomic<bool>>(false);

    // drag-start snapshots for the UiControl sizer commands
    int sidebar_drag_start_w_ = 0;
    int chat_drag_start_w_ = 0;
    bool chat_width_restored_ = false; // first-allocation chat restore done

    // Unified UI control surface (D-Bus, agent tool, HTTP — see UiControl).
    void register_ui_commands();
    nlohmann::json geometry_json() const;

    // Sizer drive path shared by the real gestures AND the UiControl
    // commands used for automated UI testing.
    void sizer_begin(const Glib::ustring& target);
    void sizer_delta(const Glib::ustring& target, double dx);
    void sizer_end(const Glib::ustring& target);
    void register_debug_actions();
    void dump_geometry(); // /tmp/monkeybyte-geometry.json for the test harness
};

} // namespace monkeybyte
