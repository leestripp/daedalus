#include "monkeybyte/ui/MainWindow.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/GitService.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/ui/ActivityBar.h"
#include "monkeybyte/ui/EditorArea.h"
#include "monkeybyte/ui/Sidebar.h"
#include "monkeybyte/ui/StatusBar.h"
#include "monkeybyte/ui/UiControl.h"
#include "monkeybyte/ui/UiGuard.h"
#include "monkeybyte/ui/chat/ChatPanel.h"
#include "monkeybyte/ui/HtmlView.h"
#include "monkeybyte/ui/panels/SettingsPanel.h"

#include <webkit/webkit.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <memory>
#include <optional>
#include <thread>

namespace monkeybyte {

namespace {
constexpr int kSidebarMin = 150, kSidebarMax = 500;
constexpr int kChatMin = 250; // no fixed max: the window bounds it (design §7)

int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// browser.* page evaluation: webkit_web_view_evaluate_javascript is async;
// run a nested main loop until the finish callback fires (bounded by a
// timeout). Safe here because UiControl handlers run on the GTK main
// thread while the caller (agent worker / HTTP thread) is already blocked
// in UiControl::execute. The script must return a JSON string; the parsed
// object is returned, or {error} on timeout / JS error / parse failure
// (a page-side {"__error": ...} is unwrapped too).
nlohmann::json eval_page_json(WebKitWebView* view, const std::string& script) {
    struct Outcome {
        std::atomic<bool> done{false};
        std::string error;
        std::string json;
    };
    // Shared with the async finish callback: on the timeout path below we
    // return while the WebKit operation is still in flight — the callback
    // fires LATER and must not write to dead stack memory. The callback
    // owns a heap-allocated copy of the shared_ptr and drops it when done,
    // keeping the Outcome alive until then.
    auto outcome = std::make_shared<Outcome>();
    webkit_web_view_evaluate_javascript(
        view, script.c_str(), -1, nullptr, nullptr, nullptr,
        [](GObject* object, GAsyncResult* res, gpointer data) {
            std::unique_ptr<std::shared_ptr<Outcome>> holder(
                static_cast<std::shared_ptr<Outcome>*>(data));
            Outcome& out = **holder;
            GError* err = nullptr;
            JSCValue* value = webkit_web_view_evaluate_javascript_finish(
                WEBKIT_WEB_VIEW(object), res, &err);
            if (err) {
                out.error = err->message;
                g_error_free(err);
            } else if (value && jsc_value_is_string(value)) {
                char* s = jsc_value_to_string(value);
                out.json = s;
                g_free(s);
            } else {
                out.error = "page script returned no string";
            }
            if (value) g_object_unref(value);
            out.done.store(true, std::memory_order_release);
        },
        new std::shared_ptr<Outcome>(outcome));

    // A g_timeout source guarantees the iteration wakes at the deadline:
    // with may_block=TRUE and no ready sources (e.g. a hung WebProcess
    // that never answers), a plain deadline check would never be reached
    // and the GTK main thread would freeze ("Not Responding").
    gboolean expired = FALSE;
    GSource* timeout = g_timeout_source_new(5000);
    g_source_set_callback(
        timeout,
        +[](gpointer data) -> gboolean {
            *static_cast<gboolean*>(data) = TRUE;
            return G_SOURCE_REMOVE;
        },
        &expired, nullptr);
    g_source_attach(timeout, nullptr); // default context — the nested loop's
    g_source_unref(timeout);           // context owns it while attached
    while (!outcome->done.load(std::memory_order_acquire) && !expired)
        g_main_context_iteration(nullptr, /*may_block=*/true);
    if (!expired) g_source_destroy(timeout); // answered in time: disarm
    if (!outcome->done.load(std::memory_order_acquire))
        return {{"error", "timed out waiting for the page to answer"}};
    if (!outcome->error.empty()) return {{"error", outcome->error}};

    nlohmann::json page = nlohmann::json::parse(outcome->json, nullptr,
                                                /*allow_exceptions=*/false);
    if (page.is_discarded())
        return {{"error", "could not parse page result"}};
    if (page.contains("__error"))
        // Page-controlled JSON: __error may not be a string — a throwing
        // get<std::string>() here rides on UiControl's try/catch instead.
        return {{"error", page["__error"].is_string()
                              ? page["__error"].get<std::string>()
                              : page["__error"].dump()}};
    return page;
}

// browser.read: pull the live page text out of a browser tab's web view.
nlohmann::json eval_page_text(WebKitWebView* view, const std::string& selector,
                              int max_chars) {
    // JSON string literal is a valid JS string literal — safe embedding.
    const std::string script =
        "(function(sel){ const el = sel ? document.querySelector(sel) : "
        "document.body; if (!el) return JSON.stringify({__error: 'no element "
        "matches the selector'}); return JSON.stringify({title: "
        "document.title || '', url: location.href || '', text: el.innerText "
        "|| ''}); })(" +
        nlohmann::json(selector.empty() ? nlohmann::json(nullptr)
                                        : nlohmann::json(selector))
            .dump() +
        ")";

    nlohmann::json page = eval_page_json(view, script);
    if (page.contains("error")) return page;
    std::string text = page.value("text", "");
    if (static_cast<int>(text.size()) > max_chars) {
        text.resize(static_cast<size_t>(max_chars));
        page["truncated"] = true;
    }
    page["text"] = text;
    page["ok"] = true;
    return page;
}
} // namespace

// GNOME "Large Text" / font scaling (org.gnome.desktop.interface
// text-scaling-factor). GTK scales only fonts from this — fixed pixel
// size-requests are NOT scaled — so we scale our resize bounds by it.
// Returns 1.0 off-GNOME or when the key is absent.
double MainWindow::text_scale_factor() {
    static double factor = -1.0;
    if (factor > 0.0) return factor;
    factor = 1.0;
    auto source = Gio::SettingsSchemaSource::get_default();
    if (source && source->lookup("org.gnome.desktop.interface", true)) {
        auto desktop = Gio::Settings::create("org.gnome.desktop.interface");
        const double f = desktop->get_double("text-scaling-factor");
        if (f > 0.0) factor = f;
    }
    return factor;
}

// Geometry persistence (§10.2): the SQLite kv store is authoritative;
// GSettings is written as a cache only. dconf does not persist in every
// environment (containers sharing a foreign session bus lose writes
// silently), so restores must come from the store, falling back to the
// GSettings value (schema default or pre-existing cache) only when no
// stored value exists yet.
int MainWindow::load_px(const char* key,
                        const Glib::RefPtr<Gio::Settings>& cache,
                        const char* cache_key) const {
    // Reached from size_allocate_vfunc — a store failure must not escape
    // GTK's allocation machinery.
    std::optional<int> stored;
    ui_guard("load_px", [&] {
        if (auto v = services_.store.get(key)) {
            int px = 0;
            const auto* end = v->data() + v->size();
            if (std::from_chars(v->data(), end, px).ec == std::errc{})
                stored = px;
        }
    });
    if (stored) return *stored;
    return cache->get_int(cache_key);
}

void MainWindow::save_px(const char* key, int value,
                         const Glib::RefPtr<Gio::Settings>& cache,
                         const char* cache_key) {
    ui_guard("save_px",
             [&] { services_.store.set(key, std::to_string(value)); });
    cache->set_int(cache_key, value); // best-effort UI cache
}

// Pixel bounds for the two docked panels, adjusted for text scaling:
// at 1.5x text the design minimum (150px) is visually much larger, so the
// floor comes down; the ceiling goes up to fit bigger text.
int MainWindow::sidebar_min() const {
    return std::max(60, static_cast<int>(kSidebarMin / text_scale_factor()));
}
int MainWindow::sidebar_max() const {
    // never more than the window can actually give
    const int avail = get_width() - 48 - 12 - 200 - chat_panel_->get_width();
    return std::max(sidebar_min(),
                    std::min(static_cast<int>(kSidebarMax * text_scale_factor()),
                             avail));
}
int MainWindow::chat_min() const {
    return std::max(120, static_cast<int>(kChatMin / text_scale_factor()));
}

MainWindow::MainWindow(AppServices& services)
    : services_(services) {
    set_title("MonkeyByte");

    const auto [win_w, win_h] = clamped_window_size();
    set_default_size(win_w, win_h);

    build_layout();
    wire_signals();
    wire_shortcuts();
    register_ui_commands();

    // Restore the persisted panel through the normal selection path: the
    // Sidebar constructor already shows it, but Settings additionally needs
    // the editor/chat paned hidden (full-width, design §2) or it renders
    // squeezed into the sidebar and overflows over the activity bar.
    on_panel_selected(static_cast<ActivityBar::Panel>(clamp(
        services_.sidebar->get_int("active-panel"), 0,
        static_cast<int>(ActivityBar::Settings))));
    control_server_.start(); // 127.0.0.1:17878 (MONKEYBYTE_CONTROL_PORT)
    update_status_branch();
    // §8 — branch switches / commits / edits happen outside the app, so
    // poll the repo on a timer (off-thread, see poll_git_status).
    git_poll_conn_ = Glib::signal_timeout().connect(
        [this] {
            poll_git_status();
            return true;
        },
        3000);
    update_title();
    status_bar_->set_model(services_.chat->get_string("selected-model"));

    // Re-assert the theme now that the full widget tree exists: the base
    // GTK theme variant (gtk-interface-color-scheme, GTK >= 4.20) must be
    // set from app side to take effect, and doing it once more here
    // guards against any startup ordering issue.
    services_.theme.reapply();
    signal_map().connect([this] { services_.theme.reapply(); });

    signal_close_request().connect(
        sigc::mem_fun(*this, &MainWindow::on_close_request), false);
}

MainWindow::~MainWindow() {
    // Stop the git poll: the timer must not fire on a dead window, and the
    // detached worker checks alive_ before touching the status bar.
    alive_->store(false);
    git_poll_conn_.disconnect();
}

void MainWindow::size_allocate_vfunc(int width, int height, int baseline) {
    Gtk::ApplicationWindow::size_allocate_vfunc(width, height, baseline);

    // Restore the chat divider against the paned's REAL allocation: the
    // save handler reads back (actual inner width − position), so the
    // restore must use the same actual width or the value drifts by a
    // handle width on every restart. While the Settings panel is active
    // the paned is hidden (width 0) — retry on the next allocation.
    if (!chat_width_restored_ && inner_paned_ && inner_paned_->get_width() > 0) {
        chat_width_restored_ = true;
        // Only a floor: there is no fixed maximum chat width (design §7) —
        // the max(200, …) below keeps the editor usable instead.
        const int chat_w =
            std::max(chat_min(), load_px("ui.chat_width", services_.chat_panel, "width"));
        inner_paned_->set_position(
            std::max(200, inner_paned_->get_width() - chat_w));
    }
}

// Clamp the restored size to the current monitor: a size saved on a
// larger display (or a multi-monitor setup) would otherwise overflow the
// screen and push the activity bar off its left edge, with no way to
// reach the panel icons again.
std::pair<int, int> MainWindow::clamped_window_size() const {
    int win_w = load_px("ui.window_width", services_.win, "width");
    int win_h = load_px("ui.window_height", services_.win, "height");
    if (auto display = Gdk::Display::get_default()) {
        if (auto monitor =
                std::dynamic_pointer_cast<Gdk::Monitor>(display->get_monitors()->get_object(0))) {
            Gdk::Rectangle geo;
            monitor->get_geometry(geo);
            if (geo.get_width() > 0 && geo.get_height() > 0) {
                win_w = std::min(win_w, geo.get_width());
                win_h = std::min(win_h, geo.get_height());
            }
        }
    }
    return {win_w, win_h};
}

void MainWindow::build_layout() {
    auto* root = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);

    activity_bar_ = Gtk::make_managed<ActivityBar>();
    sidebar_ = Gtk::make_managed<Sidebar>(services_);
    editor_area_ = Gtk::make_managed<EditorArea>(services_);
    chat_panel_ = Gtk::make_managed<ChatPanel>(services_);
    status_bar_ = Gtk::make_managed<StatusBar>();

    // Nested GtkPaned splitters (the canonical GTK resizable-panel widget —
    // see docs.gtk.org/gtk4/class.Paned.html). A divider drag only moves the
    // boundary between its two children; `resize`/`shrink` child options
    // decide who absorbs window resizes:
    //   outer: [activity + sidebar] | inner        — sidebar keeps its width
    //   inner: [editor] | chat                     — chat keeps its width
    // The editor has resize=true everywhere, so window resizes never disturb
    // the docked panels.
    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    left->append(*activity_bar_);
    sidebar_->set_hexpand(true); // fill the paned allocation; without this the
                                 // sidebar shrinks to the visible panel's
                                 // natural width (e.g. git status rows)
    left->append(*sidebar_);

    outer_paned_ = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);
    inner_paned_ = Gtk::make_managed<Gtk::Paned>(Gtk::Orientation::HORIZONTAL);

    inner_paned_->set_start_child(*editor_area_);
    inner_paned_->set_end_child(*chat_panel_);
    inner_paned_->set_resize_start_child(true);
    inner_paned_->set_shrink_start_child(true);
    inner_paned_->set_resize_end_child(false);
    inner_paned_->set_shrink_end_child(true);

    outer_paned_->set_start_child(*left);
    outer_paned_->set_end_child(*inner_paned_);
    outer_paned_->set_resize_start_child(false);
    outer_paned_->set_shrink_start_child(true);
    outer_paned_->set_resize_end_child(true);
    outer_paned_->set_shrink_end_child(true);
    outer_paned_->set_vexpand(true);

    // Initial estimate for the chat divider; the exact restore happens on
    // the first real allocation (size_allocate_vfunc) because the true
    // inner width depends on the paned handle widths, which we cannot know
    // here. Restoring only from the saved window width ignores the handles
    // and made every save→restore round-trip shift the divider by a few
    // pixels — the chat sizer visibly crept on every restart.
    const int win_w = clamped_window_size().first;
    const int sidebar_w =
        clamp(load_px("ui.sidebar_width", services_.sidebar, "width"),
              sidebar_min(), static_cast<int>(kSidebarMax * text_scale_factor()));
    const int chat_w =
        std::max(chat_min(), load_px("ui.chat_width", services_.chat_panel, "width"));
    outer_paned_->set_position(48 + sidebar_w);
    inner_paned_->set_position(std::max(200, win_w - 48 - sidebar_w - chat_w));

    // persist widths when the user finishes moving a divider
    outer_paned_->property_position().signal_changed().connect([this] {
        const int w = outer_paned_->get_position() - 48;
        // Ignore out-of-range values: those come from GTK clamping the
        // position against a too-small allocation (startup restore, window
        // shrink), not from a user drag — saving them would clobber the
        // persisted width with garbage.
        if (w >= sidebar_min() && w <= static_cast<int>(kSidebarMax * text_scale_factor()))
            save_px("ui.sidebar_width", w, services_.sidebar, "width");
    });
    inner_paned_->property_position().signal_changed().connect([this] {
        if (inner_paned_->get_width() <= 0)
            return; // not allocated yet
        const int w = inner_paned_->get_width() - inner_paned_->get_position();
        // Floor guard only (GTK clamp artifacts are tiny); no upper cap —
        // the chat panel may take as much width as the user gives it.
        if (w >= chat_min())
            save_px("ui.chat_width", w, services_.chat_panel, "width");
    });

    root->append(*outer_paned_);
    root->append(*status_bar_);
    set_child(*root);
}

void MainWindow::wire_signals() {
    activity_bar_->signal_panel_selected().connect(
        sigc::mem_fun(*this, &MainWindow::on_panel_selected));

    sidebar_->signal_open_file().connect(
        [this](const std::string& path, int line) { editor_area_->open_file(path, line); });

    sidebar_->signal_open_file_as().connect(
        [this](const std::string& path, FileOpenMode mode) {
            editor_area_->open_file_as(path, mode);
        });

    sidebar_->signal_workspace_changed().connect(
        sigc::mem_fun(*this, &MainWindow::on_workspace_changed));
    sidebar_->signal_project_type_changed().connect([this] {
        // Plan.md P6: AGENTS.md stamp rewritten; rebuild the chat agent so
        // the new type's system prompt applies from the next message.
        chat_panel_->cancel_session();
        chat_panel_->reset_agent();
    });

    // Agent-affecting settings edits (MCP, skills, tools, limits) require
    // an agent rebuild to take effect.
    sidebar_->signal_agent_config_changed().connect(
        [this] { chat_panel_->reset_agent(); });

    // Settings → Agent → Tools lists the live agent's tools when a
    // session exists.
    sidebar_->settings_panel()->set_tool_list_provider(
        [this] { return chat_panel_->agent_tools(); });

    chat_panel_->signal_model_changed().connect([this](const std::string& model) {
        services_.chat->set_string("selected-model", model);
        status_bar_->set_model(model);
    });

    // §8 — live agent stats (tokens, cached, iterations, tool calls).
    chat_panel_->signal_stats_changed().connect(
        [this](const daedalus::AgentStats& stats) {
            status_bar_->set_agent_stats(stats);
        });

    // §12.2: refresh the tree after an editor-side file delete.
    editor_area_->signal_file_deleted().connect(
        [this] { sidebar_->refresh_workspace(); });

    // Tree context-menu mutations: keep open tabs pointing at real files.
    sidebar_->signal_path_deleted().connect(
        [this](const std::string& p) { editor_area_->close_tabs_under(p); });
    sidebar_->signal_path_renamed().connect(
        [this](const std::string& old_p, const std::string& new_p) {
            editor_area_->retarget_tabs(old_p, new_p);
        });

    // Tab menu: reveal the file in the Explorer (switch to it first).
    editor_area_->signal_reveal_file().connect([this](const std::string& p) {
        on_panel_selected(ActivityBar::Workspace);
        sidebar_->reveal_in_tree(p);
    });
    // Tree follows the active editor file (selection + expansion).
    editor_area_->signal_active_file_changed().connect(
        [this](const std::string& p) { sidebar_->reveal_in_tree(p); });

    // Attach the current editor selection to outgoing chat messages so the
    // agent sees what the user highlighted without needing a tool call.
    chat_panel_->set_context_provider([this] {
        auto sel = editor_area_->selected_text();
        if (!sel) return std::string{};
        return "Selected text in " + sel->path + " (lines " +
               std::to_string(sel->start_line + 1) + "-" +
               std::to_string(sel->end_line + 1) + "):\n```\n" + sel->text +
               "\n```";
    });

    register_debug_actions();
}

// ── unified control surface (D-Bus actions, agent tool, HTTP server) ──
void MainWindow::register_ui_commands() {
    UiControl::mark_main_thread();
    auto& ctl = UiControl::instance();

    ctl.register_command("sizer.begin", "Begin a sizer drag {target: sidebar|chat}",
        [this](const nlohmann::json& a) {
            sizer_begin(a.value("target", "sidebar"));
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("sizer.delta", "Move a sizer {target, dx}",
        [this](const nlohmann::json& a) {
            sizer_delta(a.value("target", "sidebar"), a.value("dx", 0.0));
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("sizer.end", "End a sizer drag {target} (persists width)",
        [this](const nlohmann::json& a) {
            sizer_end(a.value("target", "sidebar"));
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("panel.width", "Set panel width directly {target, width}",
        [this](const nlohmann::json& a) {
            const std::string target = a.value("target", "sidebar");
            const int width = a.value("width", 260);
            if (target == "sidebar")
                outer_paned_->set_position(48 + width);
            else
                inner_paned_->set_position(
                    std::max(200, inner_paned_->get_width() - width));
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("panel.switch", "Switch sidebar panel {panel: 0-3}",
        [this](const nlohmann::json& a) {
            on_panel_selected(static_cast<ActivityBar::Panel>(a.value("panel", 0)));
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("editor.open", "Open a file {path, line?} (line: 1-based, jumps + selects)",
        [this](const nlohmann::json& a) {
            editor_area_->open_file(a.value("path", ""), a.value("line", 0));
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("editor.open_as",
        "Open a read-only preview tab {path, mode: markdown|html}",
        [this](const nlohmann::json& a) {
            const std::string mode = a.value("mode", "markdown");
            editor_area_->open_file_as(a.value("path", ""),
                mode == "html" ? FileOpenMode::HtmlPreview
                               : FileOpenMode::MarkdownPreview);
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("browser.open",
        "Open a URL in a browser tab {url} (e.g. a local dev server)",
        [this](const nlohmann::json& a) {
            const std::string url = a.value("url", "");
            if (url.empty())
                return nlohmann::json{{"error", "missing url"}};
            editor_area_->open_url(url);
            return nlohmann::json{{"ok", true}, {"url", url}};
        });
    // §6 browser tools — the agent can drive and read back the tab it
    // opened. Commands act on the active browser tab (or the most recently
    // opened one when a text tab is active); an optional {tab: url} arg
    // targets a specific open tab (matches the tab's live or initial URL).
    ctl.register_command("browser.navigate",
        "Navigate a browser tab {url, tab?}",
        [this](const nlohmann::json& a) {
            auto* view = editor_area_->browser_view_for(a.value("tab", ""));
            if (!view)
                return nlohmann::json{{"error", "no matching browser tab"}};
            const std::string url =
                HtmlView::normalize_address(a.value("url", ""));
            if (url.empty())
                return nlohmann::json{{"error", "missing url"}};
            webkit_web_view_load_uri(view, url.c_str());
            return nlohmann::json{{"ok", true}, {"url", url}};
        });
    ctl.register_command("browser.read",
        "Read the live page of a browser tab {selector?, max_chars?, tab?} "
        "— {url, title, text}; selector is a CSS selector, text is "
        "truncated to max_chars (default 8000)",
        [this](const nlohmann::json& a) {
            auto* view = editor_area_->browser_view_for(a.value("tab", ""));
            if (!view)
                return nlohmann::json{{"error", "no matching browser tab"}};
            int max_chars = a.value("max_chars", 8000);
            max_chars = clamp(max_chars, 256, 20000);
            return eval_page_text(view, a.value("selector", ""), max_chars);
        });
    // §6 browser.eval — arbitrary JS in a page the agent opened; the
    // script is evaluated as an EXPRESSION (statements need an IIFE).
    ctl.register_command("browser.eval",
        "Evaluate JavaScript in a browser tab {script, max_chars?, tab?} "
        "— {result} (JSON-decoded when possible); script is an expression, "
        "wrap statements in an IIFE. Use for DOM state, clicks, form "
        "fills, localStorage.",
        [this](const nlohmann::json& a) {
            auto* view = editor_area_->browser_view_for(a.value("tab", ""));
            if (!view)
                return nlohmann::json{{"error", "no matching browser tab"}};
            const std::string expr = a.value("script", "");
            if (expr.empty())
                return nlohmann::json{{"error", "missing script"}};
            int max_chars = a.value("max_chars", 8000);
            max_chars = clamp(max_chars, 256, 20000);

            const std::string script =
                "(function(){ try { var r = (" + expr +
                "); var s; try { s = JSON.stringify(r === undefined ? null : r);"
                " } catch (e) { s = String(r); } if (s === undefined) s = 'null';"
                " return JSON.stringify({result: s}); } catch (e) { return "
                "JSON.stringify({__error: String((e && e.message) || e)}); } })()";
            nlohmann::json page = eval_page_json(view, script);
            if (page.contains("error")) return page;

            // result arrives as a JSON-encoded string — decode it back to
            // a structured value when it parses.
            std::string raw = page.value("result", "null");
            nlohmann::json result = nlohmann::json::parse(
                raw, nullptr, /*allow_exceptions=*/false);
            if (result.is_discarded()) result = raw;
            if (static_cast<int>(raw.size()) > max_chars)
                return nlohmann::json{{"ok", true},
                                      {"result", raw.substr(0, static_cast<size_t>(max_chars))},
                                      {"truncated", true}};
            return nlohmann::json{{"ok", true}, {"result", result}};
        });
    // §6 browser.links — crawl documentation sites without re-reading
    // whole pages.
    ctl.register_command("browser.links",
        "List the links of a browser tab page {selector?, max?, tab?} — "
        "{links: [{text, href}]}, de-duplicated (default max 100); "
        "selector scopes the search root",
        [this](const nlohmann::json& a) {
            auto* view = editor_area_->browser_view_for(a.value("tab", ""));
            if (!view)
                return nlohmann::json{{"error", "no matching browser tab"}};
            int max_links = a.value("max", 100);
            max_links = clamp(max_links, 1, 500);
            const std::string selector = a.value("selector", "");

            const std::string script =
                "(function(sel, max){ var seen = {}, out = []; var nodes = "
                "document.querySelectorAll(sel ? sel + ' a' : 'a'); for (var i "
                "= 0; i < nodes.length && out.length < max; i++) { var href = "
                "nodes[i].href; if (!href || seen[href]) continue; seen[href] "
                "= true; out.push({text: (nodes[i].innerText || '').trim(), "
                "href: href}); } return JSON.stringify({links: out}); })(" +
                nlohmann::json(selector.empty() ? nlohmann::json(nullptr)
                                                : nlohmann::json(selector))
                    .dump() +
                ", " + std::to_string(max_links) + ")";
            nlohmann::json page = eval_page_json(view, script);
            if (page.contains("error")) return page;
            page["ok"] = true;
            return page;
        });
    // §6 browser.console — read back console.* and page errors captured by
    // the injected hook (ui/HtmlView), the "my page is blank, why?" loop.
    ctl.register_command("browser.console",
        "Captured console/page errors of a browser tab {max_entries?, "
        "clear?, tab?} — {entries: [{level, text}]}, newest last; "
        "clear:true empties the buffer after reading",
        [this](const nlohmann::json& a) {
            auto* view = editor_area_->browser_view_for(a.value("tab", ""));
            if (!view)
                return nlohmann::json{{"error", "no matching browser tab"}};
            auto entries = HtmlView::console_entries(view);
            int max_entries = a.value("max_entries", 50);
            max_entries = clamp(max_entries, 1, 200);
            if (static_cast<int>(entries.size()) > max_entries)
                entries.erase(entries.begin(),
                              entries.end() - max_entries);
            if (a.value("clear", false)) HtmlView::clear_console(view);
            nlohmann::json out = nlohmann::json::array();
            for (const auto& e : entries)
                out.push_back({{"level", e.level}, {"text", e.text}});
            return nlohmann::json{{"ok", true}, {"entries", out}};
        });
    // §6 browser.upload — queue local file(s) that the next file-chooser
    // request completes with, so tests/agent can drive an upload without
    // the GTK dialog: queue here, then click the page's file input via
    // browser.eval.
    ctl.register_command("browser.upload",
        "Queue local file(s) for the next file chooser in a browser tab "
        "{path | paths: [..], tab?} — after queueing, click the page's "
        "file input via browser.eval; the request completes with the "
        "queued paths instead of opening a dialog",
        [this](const nlohmann::json& a) {
            auto* view = editor_area_->browser_view_for(a.value("tab", ""));
            if (!view)
                return nlohmann::json{{"error", "no matching browser tab"}};
            std::vector<std::string> paths;
            if (a.contains("paths") && a["paths"].is_array())
                for (const auto& p : a["paths"])
                    paths.push_back(p.get<std::string>());
            else if (a.contains("path"))
                paths.push_back(a.value("path", ""));
            if (paths.empty() || paths.front().empty())
                return nlohmann::json{{"error", "missing path"}};
            for (const auto& p : paths)
                if (!std::filesystem::exists(p))
                    return nlohmann::json{{"error", "no such file: " + p}};
            HtmlView::queue_upload_files(view, paths);
            return nlohmann::json{{"ok", true}, {"files", paths}};
        });
    ctl.register_command("browser.list",
        "Open browser tabs {} — [{url, title, active}]",
        [this](const nlohmann::json&) {
            nlohmann::json tabs = nlohmann::json::array();
            for (const auto& t : editor_area_->browser_tabs())
                tabs.push_back({{"url", t.url},
                                {"title", t.title},
                                {"active", t.active}});
            return nlohmann::json{{"ok", true}, {"tabs", tabs}};
        });
    ctl.register_command("browser.close",
        "Close a browser tab {url?} — the active browser tab when omitted",
        [this](const nlohmann::json& a) {
            const std::string url = a.value("url", "");
            bool found = false;
            for (const auto& t : editor_area_->browser_tabs())
                if (url.empty() || t.url == url) found = true;
            if (!found)
                return nlohmann::json{{"error", url.empty()
                                                    ? "no browser tab open"
                                                    : "no browser tab with that url"}};
            editor_area_->close_browser_tab(url);
            return nlohmann::json{{"ok", true}};
        });
    ctl.register_command("editor.selection",
        "Text selected in the active editor {} — {path, start_line, start_col, "
        "end_line, end_col, text}; error when nothing is selected",
        [this](const nlohmann::json&) {
            auto sel = editor_area_->selected_text();
            if (!sel)
                return nlohmann::json{{"error", "no selection in the active editor"}};
            return nlohmann::json{{"ok", true},
                                  {"path", sel->path},
                                  {"start_line", sel->start_line},
                                  {"start_col", sel->start_col},
                                  {"end_line", sel->end_line},
                                  {"end_col", sel->end_col},
                                  {"text", sel->text}};
        });
    ctl.register_command("app.geometry", "Current window/panel geometry {}",
        [this](const nlohmann::json&) {
            return geometry_json();
        });
    // Diagnostic: min/natural widths of the layout children (to find widgets
    // whose content forces large panel allocations).
    ctl.register_command("app.measure", "Min/natural widths of panels {}",
        [this](const nlohmann::json&) {
            nlohmann::json out = nlohmann::json::object();
            auto probe = [&out](const char* name, Gtk::Widget& w) {
                int min_w = 0, nat_w = 0, min_b = 0, nat_b = 0;
                w.measure(Gtk::Orientation::HORIZONTAL, -1, min_w, nat_w, min_b, nat_b);
                out[name] = {{"min", min_w}, {"natural", nat_w}, {"visible", w.get_visible()}};
            };
            probe("sidebar", *sidebar_);
            probe("chat", *chat_panel_);
            probe("editor", *editor_area_);
            int i = 0;
            for (auto* child = sidebar_->get_first_child(); child;
                 child = child->get_next_sibling(), ++i)
                probe(("sidebar.child" + std::to_string(i)).c_str(), *child);
            i = 0;
            for (auto* child = chat_panel_->get_first_child(); child;
                 child = child->get_next_sibling(), ++i)
                probe(("chat.child" + std::to_string(i)).c_str(), *child);
            i = 0;
            for (auto* child = editor_area_->get_first_child(); child;
                 child = child->get_next_sibling(), ++i)
                probe(("editor.child" + std::to_string(i)).c_str(), *child);
            return out;
        });
    // Render the whole window to a PNG for visual/theme verification.
    // (Screenshots via GNOME Shell D-Bus are access-denied for scripts.)
    ctl.register_command("app.screenshot",
        "Render the window to a PNG {path?} (default /tmp/monkeybyte.png)",
        [this](const nlohmann::json& args) {
            const std::string path =
                args.value("path", std::string("/tmp/monkeybyte.png"));
            const int w = get_width(), h = get_height();
            if (w <= 0 || h <= 0)
                return nlohmann::json{{"error", "window not realized"}};
            auto* paintable = gtk_widget_paintable_new(GTK_WIDGET(gobj()));
            auto* snapshot = gtk_snapshot_new();
            gdk_paintable_snapshot(GDK_PAINTABLE(paintable), snapshot, w, h);
            g_object_unref(paintable);
            auto* node = gtk_snapshot_free_to_node(snapshot);
            if (!node)
                return nlohmann::json{{"error", "window produced no render node"}};
            auto* renderer = gtk_native_get_renderer(GTK_NATIVE(gobj()));
            const graphene_rect_t bounds =
                GRAPHENE_RECT_INIT(0, 0, (float)w, (float)h);
            auto* texture = gsk_renderer_render_texture(renderer, node, &bounds);
            gsk_render_node_unref(node);
            if (!texture)
                return nlohmann::json{{"error", "render to texture failed"}};
            const bool ok = gdk_texture_save_to_png(texture, path.c_str());
            g_object_unref(texture);
            if (!ok)
                return nlohmann::json{{"error", "could not write " + path}};
            return nlohmann::json{{"path", path}, {"width", w}, {"height", h}};
        });
    // Diagnostic: base-theme state as seen by THIS process (GTK >= 4.20
    // deprecated gtk-application-prefer-dark-theme; the base theme follows
    // gtk-interface-color-scheme: 2 = dark, 3 = light).
    ctl.register_command("app.themeprobe",
        "Dump base-theme settings + computed window color {}",
        [this](const nlohmann::json&) {
            auto* s = gtk_settings_get_default();
            gint scheme = -1;
            gboolean prefer = FALSE;
            gchar* tname = nullptr;
            g_object_get(s, "gtk-interface-color-scheme", &scheme,
                         "gtk-application-prefer-dark-theme", &prefer,
                         "gtk-theme-name", &tname, nullptr);
            nlohmann::json out = {
                {"gtk", std::to_string(gtk_get_major_version()) + "." +
                        std::to_string(gtk_get_minor_version())},
                {"color_scheme_setting", scheme},
                {"prefer_dark", static_cast<bool>(prefer)},
                {"theme_name", tname ? tname : ""},
                {"app_theme_mode", services_.theme.current()},
            };
            g_free(tname);
            // Diagnostic only: GTK 4.10 deprecated style-context color
            // lookup with no direct replacement for named theme colors, so
            // silence the deprecation locally instead of dropping the probe.
            GdkRGBA c{};
            G_GNUC_BEGIN_IGNORE_DEPRECATIONS
            if (gtk_style_context_lookup_color(
                    gtk_widget_get_style_context(GTK_WIDGET(gobj())),
                    "theme_bg_color", &c))
                out["window_theme_bg"] = {c.red, c.green, c.blue};
            G_GNUC_END_IGNORE_DEPRECATIONS
            return out;
        });
    // Diagnostic: dump the widget tree with CSS node names, so stylesheet
    // selectors can be written against reality instead of documentation.
    ctl.register_command("app.widgettree",
        "Dump widget tree with CSS node names {root? type-name substring, file?}",
        [this](const nlohmann::json& args) {
            const std::string filter = args.value("root", std::string());
            const std::string file = args.value("file", std::string());
            std::string tree;
            int count = 0;
            std::function<void(GtkWidget*, int)> walk = [&](GtkWidget* w, int depth) {
                if (!w || depth > 16 || count > 800) return;
                ++count;
                const char* type = G_OBJECT_TYPE_NAME(w);
                tree += std::string(depth * 2, ' ');
                tree += type;
                tree += " css=";
                tree += gtk_widget_get_css_name(w);
                tree += "\n";
                for (auto* c = gtk_widget_get_first_child(w); c;
                     c = gtk_widget_get_next_sibling(c))
                    walk(c, depth + 1);
            };
            walk(GTK_WIDGET(gobj()), 0);
            if (!filter.empty()) {
                // keep only lines whose widget type contains the filter,
                // plus their subtree one level deep for context
                std::string filtered;
                std::istringstream in(tree);
                std::string line;
                while (std::getline(in, line))
                    if (line.find(filter) != std::string::npos)
                        filtered += line + "\n";
                tree = std::move(filtered);
            }
            if (!file.empty()) {
                std::ofstream f(file);
                f << tree;
            }
            return nlohmann::json{{"widgets", count}, {"tree", tree.substr(0, 6000)}};
        });
    // """ + S + """14 test surface """ + DASH + """ drive a real chat turn headlessly: full
    // agent stack (provider HTTP, tools, the lot), blocking until the turn
    // ends. This is the way to smoke-test configured providers/models.
    ctl.register_command("chat.send",
        "Send a chat prompt, wait for the turn {text, model?, timeout_s?}",
        [this](const nlohmann::json& args) {
            const std::string text = args.value("text", std::string());
            const std::string model = args.value("model", std::string());
            const int timeout_s =
                std::clamp(args.value("timeout_s", 120), 5, 600);
            return chat_panel_->send_message_sync(text, model, timeout_s);
        });
    ctl.register_command("app.commands", "List available control commands {}",
        [](const nlohmann::json&) {
            const auto& c = UiControl::instance();
            nlohmann::json out = nlohmann::json::object();
            for (const auto& name : c.list_commands())
                out[name] = c.describe(name);
            return out;
        });
}

nlohmann::json MainWindow::geometry_json() const {
    int req_w = 0, req_h = 0;
    sidebar_->get_size_request(req_w, req_h);
    const int sidebar_req = req_w;
    chat_panel_->get_size_request(req_w, req_h);
    return {
        {"window", get_width()},
        {"sidebar", sidebar_->get_width()},
        {"sidebar_req", sidebar_req},
        {"chat", chat_panel_->get_width()},
        {"chat_req", req_w},
        {"text_scale", text_scale_factor()},
    };
}

// ── sizer drive path (UiControl commands) ───────────────────────────
// GtkPaned owns the real drag; these drive the same positions for tests.
void MainWindow::sizer_begin(const Glib::ustring& target) {
    if (target == "sidebar")
        sidebar_drag_start_w_ = outer_paned_->get_position();
    else
        chat_drag_start_w_ = inner_paned_->get_width() - inner_paned_->get_position();
}

void MainWindow::sizer_delta(const Glib::ustring& target, double dx) {
    if (target == "sidebar") {
        outer_paned_->set_position(
            std::max(48, sidebar_drag_start_w_ + static_cast<int>(dx)));
    } else {
        const int chat_w = chat_drag_start_w_ - static_cast<int>(dx);
        inner_paned_->set_position(
            std::max(200, inner_paned_->get_width() - chat_w));
    }
}

void MainWindow::sizer_end(const Glib::ustring& target) {
    // positions already persist via property_position::changed (build_layout)
    if (target == "sidebar")
        save_px("ui.sidebar_width", std::max(0, outer_paned_->get_position() - 48),
                services_.sidebar, "width");
    else
        save_px("ui.chat_width",
                std::max(0, inner_paned_->get_width() - inner_paned_->get_position()),
                services_.chat_panel, "width");
}

// ── D-Bus debug hooks (org.gtk.Actions over the session bus) ────────
// Drive with e.g.:
//   gdbus call --session --dest io.github.monkeybyte
//     --object-path /io/github/monkeybyte --method org.gtk.Actions.Activate
//     win.sizer-delta "[('sidebar', <40.0>)]" {}
void MainWindow::register_debug_actions() {
    auto& ctl = UiControl::instance();
    auto string_action = [this, &ctl](const Glib::ustring& name,
                                      const char* command) {
        add_action_with_parameter(
            name, Glib::VariantType("s"),
            [&ctl, command](const Glib::VariantBase& param) {
                ui_guard("dbus sizer action", [&] {
                    const auto target =
                        Glib::VariantBase::cast_dynamic<Glib::Variant<Glib::ustring>>(param).get();
                    ctl.execute(command, {{"target", std::string(target)}});
                });
            });
    };
    string_action("sizer-begin", "sizer.begin");
    string_action("sizer-end", "sizer.end");

    add_action_with_parameter(
        "sizer-delta", Glib::VariantType("(sd)"),
        [&ctl](const Glib::VariantBase& param) {
            ui_guard("dbus sizer-delta", [&] {
                auto tuple = Glib::VariantBase::cast_dynamic<Glib::VariantContainerBase>(param);
                Glib::Variant<Glib::ustring> target;
                Glib::Variant<double> dx;
                tuple.get_child(target, 0);
                tuple.get_child(dx, 1);
                ctl.execute("sizer.delta",
                            {{"target", std::string(target.get())}, {"dx", dx.get()}});
            });
        });

    add_action("dump-geometry", [this] { dump_geometry(); });
}

void MainWindow::dump_geometry() {
    const std::string json = geometry_json().dump() + "\n";
    auto* f = std::fopen("/tmp/monkeybyte-geometry.json", "w");
    if (f) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
    }
}

void MainWindow::wire_shortcuts() {
    auto controller = Gtk::ShortcutController::create();
    controller->set_scope(Gtk::ShortcutScope::GLOBAL);

    auto add = [&](const char* trigger, std::function<void()> fn) {
        auto action = Gtk::CallbackAction::create(
            [fn = std::move(fn)](Gtk::Widget&, const Glib::VariantBase&) {
                fn();
                return true;
            });
        controller->add_shortcut(Gtk::Shortcut::create(
            Gtk::ShortcutTrigger::parse_string(trigger), action));
    };

    add("<Control>1", [this] { on_panel_selected(ActivityBar::Workspace); });
    add("<Control>2", [this] { on_panel_selected(ActivityBar::Search); });
    add("<Control>3", [this] { on_panel_selected(ActivityBar::Git); });
    add("<Control>4", [this] { on_panel_selected(ActivityBar::Settings); });
    add("<Control>s", [this] { editor_area_->save_active(); });
    add("<Control>w", [this] { editor_area_->close_active(); });

    add_controller(controller);
}

void MainWindow::on_panel_selected(ActivityBar::Panel panel) {
    activity_bar_->set_active(panel);
    services_.sidebar->set_int("active-panel", static_cast<int>(panel));

    // Settings takes the full client width (design §2).
    const bool settings_active = (panel == ActivityBar::Settings);
    inner_paned_->set_visible(!settings_active);

    sidebar_->show_panel(panel);
}

void MainWindow::on_workspace_changed(const std::string& path) {
    // Design §12.1: cancel a running agent before switching workspaces.
    chat_panel_->cancel_session();
    chat_panel_->clear_history();

    ui_guard("on_workspace_changed",
             [&] { services_.store.set("workspace", path); });
    services_.files.set_workspace(path);
    sidebar_->refresh_workspace();
    update_status_branch();
    update_title();
}

void MainWindow::update_title() {
    const auto& ws = services_.files.workspace();
    set_title(ws.empty() ? "MonkeyByte"
                         : "MonkeyByte — " + ws.filename().string());
}

void MainWindow::update_status_branch() {
    const auto& ws = services_.files.workspace();
    if (ws.empty()) {
        status_bar_->set_branch_info("");
        return;
    }
    // GitService::status throws if git can't be spawned (unlike the guarded
    // GitPanel twin) — degrade to no branch info instead of crashing the
    // signal that triggered the refresh.
    GitService::Status st;
    try {
        st = GitService::status(ws);
    } catch (const std::exception&) {
        status_bar_->set_branch_info("");
        return;
    }
    if (!st.is_repo) {
        status_bar_->set_branch_info("Not a git repo");
        return;
    }
    // §8 — branch + dirty count (read-only; same refresh triggers as before).
    std::string text = st.branch;
    if (!st.changes.empty())
        text += " · " + std::to_string(st.changes.size()) +
                (st.changes.size() == 1 ? " change" : " changes");
    status_bar_->set_branch_info(text);
}

// §8 — timer tick (every 3 s). GitService::status spawns git subprocesses,
// which can block on large repos, so the work runs on a detached thread
// and only the label update is marshaled back to the main loop. Ticks
// skipped while a previous poll is still running; a transient spawn
// failure keeps the previous label for this tick.
void MainWindow::poll_git_status() {
    const auto& ws = services_.files.workspace();
    if (ws.empty()) return; // label already hidden by the workspace path
    bool expected = false;
    if (!git_poll_in_flight_->compare_exchange_strong(expected, true))
        return;
    // try/catch: std::thread construction can throw std::system_error, and
    // an exception escaping this GLib timeout would abort the process.
    try {
        std::thread([this, alive = alive_, in_flight = git_poll_in_flight_, ws] {
            std::optional<std::string> text; // nullopt = keep the old label
            try {
                const auto st = GitService::status(ws);
                if (st.is_repo) {
                    std::string t = st.branch;
                    if (!st.changes.empty())
                        t += " · " + std::to_string(st.changes.size()) +
                             (st.changes.size() == 1 ? " change" : " changes");
                    text = std::move(t);
                } else {
                    text = "Not a git repo";
                }
            } catch (const std::exception&) {
                // git spawn failed — leave the label alone this tick
            }
            Glib::signal_idle().connect_once([this, alive, text] {
                if (*alive && text) status_bar_->set_branch_info(*text);
            });
            in_flight->store(false);
        }).detach();
    } catch (const std::system_error& e) { // thread spawn failed
        Logger::instance().error(std::string("poll_git_status: ") + e.what());
        git_poll_in_flight_->store(false); // let the next tick retry
    }
}

bool MainWindow::on_close_request() {
    // Save the actual current size, not the startup default — otherwise user
    // resizes never persist and a stale oversized value round-trips forever.
    const int w = get_width();
    const int h = get_height();
    if (w > 0 && h > 0) {
        save_px("ui.window_width", w, services_.win, "width");
        save_px("ui.window_height", h, services_.win, "height");
    }
    // Panel widths already persist via the position::changed handlers, but
    // exit is the moment the user expects state to be committed — save the
    // final positions explicitly (same range guards: skip GTK clamping
    // artifacts and a hidden/zeroed chat paned).
    const int sidebar_w = outer_paned_->get_position() - 48;
    if (sidebar_w >= sidebar_min() &&
        sidebar_w <= static_cast<int>(kSidebarMax * text_scale_factor()))
        save_px("ui.sidebar_width", sidebar_w, services_.sidebar, "width");
    if (inner_paned_->get_width() > 0) {
        const int chat_w = inner_paned_->get_width() - inner_paned_->get_position();
        if (chat_w >= chat_min())
            save_px("ui.chat_width", chat_w, services_.chat_panel, "width");
    }
    return false; // allow close
}

} // namespace monkeybyte
