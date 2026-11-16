#include "monkeybyte/ui/HtmlView.h"

#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/backend/MarkdownToHtml.h"

#include <webkit/webkit.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <filesystem>
#include <utility>
#include <nlohmann/json.hpp>

namespace monkeybyte {

namespace {

// Same wrap pattern EditorArea uses for its C-API GtkSourceView: the widget
// is owned by GTK once a container sinks its floating reference.
// Google (YouTube) sniffs the UA: WebKitGTK's stock "Version/60.5 Safari"
// string is served a broken ES5 kevlar bundle whose bootstrap crashes
// (_._DumpException is not a function) before the app boots. Present a
// mainstream desktop Chrome UA instead; device presets still override it.
constexpr char kDesktopUserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";

// Popup hook installed by EditorArea (HtmlView::set_popup_handler): the
// "create" signal handler hands it a new related view to adopt into a tab.
std::function<void(WebKitWebView*)> g_popup_handler;

// --- Downloads (context-menu "Save Image As..." etc.) ------------------
//
// WebKit's default context menu offers download actions ("Save Image
// As...", "Download Linked File", "Save Video/Audio As..."). Picking one
// starts a WebKitDownload announced by WebKitNetworkSession's
// "download-started" — with no handler the download is never given a
// destination and dies silently: no save dialog, no file. Hook the shared
// network session once and run a save dialog per download.
constexpr char kDownloadsHookedKey[] = "mb-downloads-hooked";

void on_decide_destination(WebKitDownload* download,
                           const gchar* suggested_filename) {
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Save File");
    if (suggested_filename && *suggested_filename)
        dialog->set_initial_name(suggested_filename);
    if (const std::string downloads =
            Glib::get_user_special_dir(Glib::UserDirectory::DOWNLOAD);
        !downloads.empty())
        dialog->set_initial_folder(Gio::File::create_for_path(downloads));

    Gtk::Window* toplevel = nullptr;
    if (auto app = std::dynamic_pointer_cast<Gtk::Application>(
            Gio::Application::get_default()))
        toplevel = app->get_active_window();

    // Run the async GtkFileDialog to completion inside a nested main loop
    // so the destination is decided BEFORE this handler returns. The
    // documented async contract (return TRUE from decide-destination, call
    // webkit_download_set_destination() later) stalls the download on
    // webkitgtk-6.0 2.52: the destination gets set from the dialog
    // callback, then nothing — no created-destination, finished, or failed
    // ever fires. Setting the destination synchronously here is the
    // pre-2.40 contract, which works; the nesting is exactly what
    // gtk_dialog_run() did for years.
    bool completed = false;
    Glib::RefPtr<Gio::File> chosen;
    auto done = [dialog, &completed,
                 &chosen](Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            chosen = dialog->save_finish(result);
        } catch (const Gtk::DialogError&) {
            // cancelled
        } catch (const Glib::Error&) {
            // failed
        }
        completed = true;
    };
    if (toplevel)
        dialog->save(*toplevel, done);
    else
        dialog->save(done);
    while (!completed)
        g_main_context_iteration(nullptr, TRUE);

    if (!chosen) {
        webkit_download_cancel(download);
        return;
    }

    // webkitgtk-6.0 2.52 never transfers file:// download sources: the
    // destination gets set (sync or async) and the download then stalls
    // forever — no created-destination, finished, or failed ever fires.
    // For a local source we don't need WebKit's transfer at all: copy the
    // file directly and cancel the (doomed) download. The GTK save dialog
    // already confirmed any overwrite, so OVERWRITE is correct here.
    const char* src_uri =
        webkit_download_get_request(download)
            ? webkit_uri_request_get_uri(webkit_download_get_request(download))
            : nullptr;
    if (src_uri && g_str_has_prefix(src_uri, "file://")) {
        auto source = Gio::File::create_for_uri(src_uri);
        try {
            source->copy(chosen, Gio::File::CopyFlags::OVERWRITE);
        } catch (const Glib::Error& e) {
            Logger::instance().error("download direct copy FAILED: " +
                                     std::string(e.what()));
        }
        webkit_download_cancel(download);
        return;
    }

    // allow-overwrite: the GTK save dialog already confirms overwrites
    // with the user — without this, WebKit vetoes the confirmed overwrite
    // and the download fails (allow-overwrite defaults to FALSE).
    webkit_download_set_allow_overwrite(download, TRUE);
    webkit_download_set_destination(download, chosen->get_uri().c_str());
}

void hook_downloads(WebKitWebView* view) {
    auto* session = webkit_web_view_get_network_session(view);
    if (!session ||
        g_object_get_data(G_OBJECT(session), kDownloadsHookedKey))
        return;
    g_object_set_data(G_OBJECT(session), kDownloadsHookedKey,
                      GINT_TO_POINTER(1));
    g_signal_connect(
        session, "download-started",
        G_CALLBACK(+[](WebKitNetworkSession*, WebKitDownload* download,
                       gpointer) {
            // "failed" also fires for our own webkit_download_cancel()
            // after a direct file:// copy ("User cancelled the download")
            // — that one is expected, log only genuine failures.
            g_signal_connect(
                download, "failed",
                G_CALLBACK(+[](WebKitDownload*, GError* error, gpointer) {
                    if (error && g_error_matches(error, WEBKIT_DOWNLOAD_ERROR,
                                                 WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
                        return;
                    Logger::instance().error(
                        std::string("download FAILED: ") +
                        (error ? error->message : "?"));
                }),
                nullptr);
            g_signal_connect(download, "decide-destination",
                             G_CALLBACK(+[](WebKitDownload* d,
                                            const gchar* suggested_filename,
                                            gpointer) -> gboolean {
                                 on_decide_destination(d,
                                                       suggested_filename);
                                 return TRUE;
                             }),
                             nullptr);
        }),
        nullptr);
}

Gtk::Widget* wrap_view(WebKitWebView* web_view) {
    hook_downloads(web_view); // every view passes through here
    auto* widget = GTK_WIDGET(web_view);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    return Glib::wrap(widget);
}

// Shared by the markdown / html-content / fountain document views: http(s)
// navigation leaves the app (system browser), everything else (file://,
// anchors, about:) is handled by WebKit itself.
void attach_external_link_policy(WebKitWebView* view) {
    g_signal_connect(view, "decide-policy",
                     G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* d,
                                    WebKitPolicyDecisionType type,
                                    gpointer) -> gboolean {
                         if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
                             return FALSE;
                         auto* nav = webkit_navigation_policy_decision_get_navigation_action(
                             WEBKIT_NAVIGATION_POLICY_DECISION(d));
                         WebKitURIRequest* req =
                             webkit_navigation_action_get_request(nav);
                         const char* uri =
                             req ? webkit_uri_request_get_uri(req) : nullptr;
                         if (!uri) return FALSE;
                         const std::string u = uri;
                         if (u.rfind("http://", 0) != 0 &&
                             u.rfind("https://", 0) != 0)
                             return FALSE;
                         webkit_policy_decision_ignore(d);
                         Gio::AppInfo::launch_default_for_uri(u);
                         return TRUE;
                     }),
                     nullptr);
}

// --- Fountain preview zoom (Plan.md P4) ----------------------------------

constexpr double kFountainZoomMin = 0.5;
constexpr double kFountainZoomMax = 4.0;
constexpr double kFountainZoomStep = 0.25;

// Session-shared Fountain zoom; < 0 = not chosen yet, so the next preview
// starts at the display-aware default. Every change (any fountain tab)
// updates it, so newly opened previews inherit the user's preference.
double g_fountain_zoom = -1.0;

// Lives as long as the fountain page widget (freed via g_object data).
// Unlike BrowserState no weak refs are needed: the bar and the web view
// are siblings in the same box and are destroyed together on tab close.
struct FountainState {
    WebKitWebView* view;
    Gtk::Button* zoom_label;
    double home_zoom; // display-aware default, the reset target
};

constexpr char kFountainStateKey[] = "mb-fountain-state";

void fountain_set_zoom(FountainState* state, double zoom) {
    zoom = std::clamp(std::round(zoom / 0.05) * 0.05, kFountainZoomMin,
                      kFountainZoomMax);
    webkit_web_view_set_zoom_level(state->view, zoom);
    g_fountain_zoom = zoom;
    state->zoom_label->set_label(
        std::to_string(static_cast<int>(std::lround(zoom * 100.0))) + "%");
}

// Physical DPI and scale factor of the monitor the app window sits on
// (falling back to the first monitor). dpi = 0 when the physical size is
// unknown (headless, odd drivers), letting the caller assume standard
// density.
std::pair<double, double> monitor_density() {
    auto display = Gdk::Display::get_default();
    if (!display) return {0.0, 1.0};
    Glib::RefPtr<Gdk::Monitor> mon;
    if (auto app = std::dynamic_pointer_cast<Gtk::Application>(
            Gio::Application::get_default()))
        if (auto* win = app->get_active_window())
            if (auto surface = win->get_surface())
                mon = display->get_monitor_at_surface(surface);
    if (!mon) {
        auto monitors = display->get_monitors();
        if (monitors->get_n_items() > 0)
            mon = std::dynamic_pointer_cast<Gdk::Monitor>(
                monitors->get_object(0));
    }
    if (!mon) return {0.0, 1.0};
    const double scale = mon->get_scale();
    Gdk::Rectangle geo;
    mon->get_geometry(geo);
    const int width_mm = mon->get_width_mm();
    if (width_mm <= 0 || geo.get_width() <= 0) return {0.0, scale};
    // get_geometry() is in logical (application) pixels; multiply by the
    // scale factor to get the PHYSICAL dpi the zoom formula expects -
    // logical_px/mm is already compositor-adjusted, dividing by the scale
    // again would double-count it.
    return {geo.get_width() * scale / (width_mm / 25.4), scale};
}

// Desktop text scaling (GNOME "Large Text" etc.); 1.0 where the setting
// doesn't exist. The schema lookup is guarded: Gio::Settings::create()
// aborts on an unknown schema id, and non-GNOME desktops (KDE, Sway...)
// may not ship org.gnome.desktop.interface at all.
double text_scaling_factor() {
    GSettingsSchemaSource* src = g_settings_schema_source_get_default();
    if (!src) return 1.0;
    GSettingsSchema* schema = g_settings_schema_source_lookup(
        src, "org.gnome.desktop.interface", TRUE);
    if (!schema) return 1.0;
    const bool has_key =
        g_settings_schema_has_key(schema, "text-scaling-factor");
    g_settings_schema_unref(schema);
    if (!has_key) return 1.0;
    try {
        const double v = Gio::Settings::create("org.gnome.desktop.interface")
                             ->get_double("text-scaling-factor");
        return v > 0 ? v : 1.0;
    } catch (...) {
        return 1.0;
    }
}

// Zoom bar (- / % / +) over the web view, plus Ctrl +/- /0 shortcuts.
Gtk::Widget* fountain_page(WebKitWebView* view) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_vexpand(true);

    const auto [dpi, scale] = monitor_density();
    const double home =
        HtmlView::fountain_default_zoom(dpi, scale, text_scaling_factor());

    auto* bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    bar->set_margin(4);
    bar->set_halign(Gtk::Align::END);
    bar->add_css_class("fountain-zoom-bar");
    auto* zoom_out = Gtk::make_managed<Gtk::Button>("\u2212"); // minus sign
    auto* zoom_label = Gtk::make_managed<Gtk::Button>();
    auto* zoom_in = Gtk::make_managed<Gtk::Button>("+");
    for (auto* b : {zoom_out, zoom_label, zoom_in}) b->set_has_frame(false);
    zoom_out->set_tooltip_text("Zoom out (Ctrl+-)");
    zoom_in->set_tooltip_text("Zoom in (Ctrl++)");
    zoom_label->set_tooltip_text(
        "Reset to the default zoom for this display (Ctrl+0)");
    bar->append(*zoom_out);
    bar->append(*zoom_label);
    bar->append(*zoom_in);
    box->append(*bar);
    box->append(*wrap_view(view));

    auto* state = new FountainState{view, zoom_label, home};
    g_object_set_data_full(G_OBJECT(box->gobj()), kFountainStateKey, state,
                           [](gpointer p) {
                               delete static_cast<FountainState*>(p);
                           });

    zoom_out->signal_clicked().connect([state] {
        fountain_set_zoom(state,
                          webkit_web_view_get_zoom_level(state->view) -
                              kFountainZoomStep);
    });
    zoom_in->signal_clicked().connect([state] {
        fountain_set_zoom(state,
                          webkit_web_view_get_zoom_level(state->view) +
                              kFountainZoomStep);
    });
    zoom_label->signal_clicked().connect(
        [state] { fountain_set_zoom(state, state->home_zoom); });

    // Capture phase: the web view consumes most key events, so intercept
    // the zoom chords before they reach it.
    auto keys = Gtk::EventControllerKey::create();
    keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
    keys->signal_key_pressed().connect(
        [state](guint keyval, guint, Gdk::ModifierType mods) {
            if ((mods & Gdk::ModifierType::CONTROL_MASK) !=
                Gdk::ModifierType::CONTROL_MASK)
                return false;
            const double z = webkit_web_view_get_zoom_level(state->view);
            switch (keyval) {
            case GDK_KEY_plus:
            case GDK_KEY_equal: // unshifted '+' on most layouts
            case GDK_KEY_KP_Add:
                fountain_set_zoom(state, z + kFountainZoomStep);
                return true;
            case GDK_KEY_minus:
            case GDK_KEY_underscore:
            case GDK_KEY_KP_Subtract:
                fountain_set_zoom(state, z - kFountainZoomStep);
                return true;
            case GDK_KEY_0:
            case GDK_KEY_KP_0:
                fountain_set_zoom(state, state->home_zoom);
                return true;
            default:
                return false;
            }
        },
        false);
    box->add_controller(keys);

    fountain_set_zoom(state, g_fountain_zoom > 0 ? g_fountain_zoom : home);
    return box;
}

// Lives as long as the browser page widget (freed via g_object data).
struct BrowserState {
    WebKitWebView* view;
    Gtk::Entry* address;
    Gtk::Button* back;
    Gtk::Button* forward;
    // §6 browser.console — bounded capture of console.* and page errors,
    // cleared on every navigation start.
    std::deque<HtmlView::ConsoleEntry> console;
    // §6 browser.upload — local paths queued via UiControl; the next
    // run-file-chooser request completes with these instead of opening
    // the default GTK dialog. Consumed on first use.
    std::vector<std::string> pending_upload;
    // §6 screen-size presets (📱 nav-bar menu): 0×0 = fit to window.
    int dev_w = 0, dev_h = 0;
    const char* dev_ua = nullptr; // mobile UA override, nullptr = default
    std::string dev_id;
    std::string dev_label;
    bool dev_rotated = false;
    Gtk::MenuButton* device_btn = nullptr;
};

// Null a BrowserState widget pointer the moment the widget's GObject is
// finalized. Tab close destroys the nav bar BEFORE the WebKitWebView, and
// WebKit can still emit load-changed during its own teardown -- without
// this guard the handler dereferences a dead widget (intermittent
// segfault on closing a browser tab mid-load). try/catch cannot help here:
// this is a C-level use-after-free, not a C++ exception.
template <typename T>
void track_widget(T** slot, T* w) {
    g_object_weak_ref(G_OBJECT(w->gobj()),
                      [](gpointer data, GObject*) {
                          *static_cast<T**>(data) = nullptr;
                      },
                      slot);
}

constexpr char kBrowserStateKey[] = "mb-browser-state";
constexpr std::size_t kMaxConsoleEntries = 200;

// Injected at document start (§6 browser.console): mirror console.error /
// warn / log and window error events to the mbConsole script message
// handler, which buffers them in BrowserState for the agent to read back.
constexpr char kConsoleHookScript[] = R"JS(
(function() {
    if (window.__mbConsoleHooked) return;
    window.__mbConsoleHooked = true;
    var pending = [];
    var dropped = 0;
    var flushScheduled = false;
    function flush() {
        flushScheduled = false;
        var entries = pending;
        pending = [];
        if (dropped > 0) {
            entries.unshift({level: 'warn', text: '[monkeybyte] dropped ' +
                dropped + ' console messages during a logging burst'});
            dropped = 0;
        }
        try {
            window.webkit.messageHandlers.mbConsole.postMessage(
                JSON.stringify(entries));
        } catch (e) {}
    }
    function post(level, args) {
        try {
            var text = Array.prototype.map.call(args, function(a) {
                if (typeof a === 'string') return a;
                try { return JSON.stringify(a); } catch (e) { return String(a); }
            }).join(' ');
            // Bound retained text without splitting a UTF-16 surrogate pair.
            if (text.length > 4096) {
                var end = 4096;
                var last = text.charCodeAt(end - 1);
                if (last >= 0xD800 && last <= 0xDBFF) --end;
                text = text.slice(0, end) + '…';
            }
            // One IPC delivery per burst, not one per console call. Keep the
            // newest entries just like the native bounded capture buffer.
            if (pending.length >= 100) {
                pending.shift();
                ++dropped;
            }
            pending.push({level: level, text: text});
            if (!flushScheduled) {
                flushScheduled = true;
                setTimeout(flush, 50);
            }
        } catch (e) {}
    }
    ['error', 'warn', 'log'].forEach(function(level) {
        var orig = console[level] && console[level].bind(console);
        console[level] = function() {
            post(level, arguments);
            if (orig) orig.apply(null, arguments);
        };
    });
    window.addEventListener('error', function(e) {
        post('error', ['[pageerror] ' + (e.message || '') + ' @ ' +
                       (e.filename || '') + ':' + (e.lineno || 0)]);
    });
    window.addEventListener('unhandledrejection', function(e) {
        post('error', ['[unhandledrejection] ' + String(e.reason)]);
    });
})();
)JS";

constexpr std::size_t kMaxConsoleTextBytes = 16 * 1024;

void append_console_entry(BrowserState* state, const nlohmann::json& entry) {
    // Page-controlled input: any site can post arbitrary JSON to this
    // handler — entry.value() would throw type_error through the C callback.
    auto get_str = [&entry](const char* key, const char* fallback) {
        const auto it = entry.find(key);
        return (it != entry.end() && it->is_string()) ? it->get<std::string>()
                                                      : std::string(fallback);
    };
    auto level = get_str("level", "log");
    if (level != "error" && level != "warn" && level != "log") level = "log";
    auto text = get_str("text", "");
    // Also cap direct posts that bypass the injected script. JSON parsing
    // has validated UTF-8, so backing up to a lead byte preserves the text.
    if (text.size() > kMaxConsoleTextBytes) {
        auto end = kMaxConsoleTextBytes;
        while (end > 0 &&
               (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;
        text.resize(end);
        text += "…";
    }
    if (state->console.size() >= kMaxConsoleEntries)
        state->console.pop_front();
    state->console.push_back({std::move(level), std::move(text)});
}

void on_console_message(WebKitUserContentManager*, JSCValue* message,
                        gpointer data) {
    auto* state = static_cast<BrowserState*>(data);
    if (!jsc_value_is_string(message)) return;
    char* raw = jsc_value_to_string(message);
    const auto payload = nlohmann::json::parse(raw, nullptr,
                                               /*allow_exceptions=*/false);
    g_free(raw);
    if (payload.is_discarded()) return;
    if (!payload.is_array()) {
        append_console_entry(state, payload); // direct/legacy single entry
        return;
    }
    // A hostile page can post an arbitrarily large array directly. Only the
    // newest entries can survive the native buffer, so ignore the rest.
    const auto first = payload.size() > kMaxConsoleEntries
                           ? payload.size() - kMaxConsoleEntries
                           : 0;
    for (auto i = first; i < payload.size(); ++i)
        append_console_entry(state, payload[i]);
}

void hook_console(WebKitWebView* view, BrowserState* state) {
    WebKitUserContentManager* ucm =
        webkit_web_view_get_user_content_manager(view);
    // Connect before registering (WebKit docs: avoids a race between
    // registration and the first emitted message).
    g_signal_connect(ucm, "script-message-received::mbConsole",
                     G_CALLBACK(on_console_message), state);
    webkit_user_content_manager_register_script_message_handler(
        ucm, "mbConsole", nullptr);
    WebKitUserScript* script = webkit_user_script_new(
        kConsoleHookScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
}

// §6 screen-size presets. ua = nullptr keeps WebKit's default (desktop)
// UA; phone presets spoof a mobile UA for sites that sniff the UA string
// instead of using viewport-width media queries.
struct DevicePreset {
    const char* label;
    int w, h;
    const char* ua;
};

constexpr char kIphoneUa[] =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 "
    "Mobile/15E148 Safari/604.1";
constexpr char kAndroidUa[] =
    "Mozilla/5.0 (Linux; Android 14; Pixel 8) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0 "
    "Mobile Safari/537.36";

// Index-aligned with the menu sections built in browser_page().
constexpr HtmlView::DevicePreset kDevicePresets[] = {
    // Phones
    {"iphone-se", "iPhone SE — 375×667", 375, 667, kIphoneUa},
    {"iphone-14-15", "iPhone 14/15 — 390×844", 390, 844, kIphoneUa},
    {"iphone-pro-max", "iPhone Pro Max — 430×932", 430, 932, kIphoneUa},
    {"galaxy-s", "Galaxy S — 360×800", 360, 800, kAndroidUa},
    {"pixel-8", "Pixel 8 — 412×915", 412, 915, kAndroidUa},
    // Tablets (modern tablets default to desktop UA — no spoof)
    {"ipad-mini", "iPad Mini — 744×1133", 744, 1133, nullptr},
    {"ipad", "iPad — 768×1024", 768, 1024, nullptr},
    {"ipad-pro-11", "iPad Pro 11\u2033 — 834×1194", 834, 1194, nullptr},
    {"galaxy-tab-s", "Galaxy Tab S — 800×1280", 800, 1280, nullptr},
    // Laptops
    {"laptop-1366x768", "Laptop — 1366×768", 1366, 768, nullptr},
    {"laptop-1440x900", "Laptop — 1440×900", 1440, 900, nullptr},
    {"laptop-1536x864", "Laptop — 1536×864", 1536, 864, nullptr},
    // Desktops
    {"desktop-1920x1080", "Desktop — 1920×1080", 1920, 1080, nullptr},
    {"desktop-2560x1440", "Desktop — 2560×1440", 2560, 1440, nullptr},
};
constexpr int kPresetPhones = 0, kPresetTablets = 5, kPresetLaptops = 9,
              kPresetDesktops = 12, kPresetCount = 14;

// Constrain (or free, w==0) the view widget inside its centered container.
// Clamp layout for the browser web view (§6): allocates the child at
// exactly min(size-request, available), centered, and reports the request
// as its own size so the enclosing GtkScrolledWindow scrolls presets that
// are LARGER than the tab. Needed because WebKit's natural size sticks to
// the last layout width — GtkAlign::CENTER on the view itself allocates
// the stale natural size after shrinking (which broke Fit to Window).
void clamp_measure(GtkWidget* w, GtkOrientation o, int, int* min, int* nat,
                   int* min_baseline, int* nat_baseline) {
    auto* child = gtk_widget_get_first_child(w);
    int req_w = -1, req_h = -1;
    if (child) gtk_widget_get_size_request(child, &req_w, &req_h);
    const int r = o == GTK_ORIENTATION_HORIZONTAL ? req_w : req_h;
    *min = *nat = r > 0 ? r : 0;
    if (min_baseline) *min_baseline = -1;
    if (nat_baseline) *nat_baseline = -1;
}

void clamp_allocate(GtkWidget* w, int width, int height, int baseline) {
    auto* child = gtk_widget_get_first_child(w);
    if (!child) return;
    int req_w = -1, req_h = -1;
    gtk_widget_get_size_request(child, &req_w, &req_h);
    const int cw = req_w > 0 ? std::min(req_w, width) : width;
    const int ch = req_h > 0 ? std::min(req_h, height) : height;
    const GtkAllocation alloc{(width - cw) / 2, (height - ch) / 2, cw, ch};
    gtk_widget_size_allocate(child, &alloc, baseline);
}

// The WebKitGTK viewport IS the widget size, so width-based media queries
// respond immediately; reload applies a UA change for UA-sniffing sites.
// The size request is the ONLY layout channel — the clamp layout above
// reads it directly (expand/alignment flags are irrelevant inside it).
HtmlView::DeviceSize apply_device(BrowserState* s, int w, int h,
                                  const char* ua, const std::string& id,
                                  const std::string& label, bool rotated) {
    s->dev_w = w;
    s->dev_h = h;
    s->dev_ua = ua;
    s->dev_id = id;
    s->dev_label = label;
    s->dev_rotated = rotated;
    auto* widget = GTK_WIDGET(s->view);
    gtk_widget_set_size_request(widget, w > 0 ? w : -1, h > 0 ? h : -1);
    WebKitSettings* settings = webkit_web_view_get_settings(s->view);
    webkit_settings_set_user_agent(settings, ua ? ua : kDesktopUserAgent);
    if (s->device_btn) {
        const std::string suffix = rotated ? " (rotated)" : "";
        s->device_btn->set_tooltip_text(
            label.empty() ? "Screen size presets"
                          : "Screen size: " + label + suffix);
    }
    webkit_web_view_reload(s->view);
    return {w, h, s->dev_id, s->dev_label, s->dev_rotated};
}

BrowserState* browser_state(WebKitWebView* view) {
    if (!view) return nullptr;
    return static_cast<BrowserState*>(
        g_object_get_data(G_OBJECT(view), kBrowserStateKey));
}

// Vertical box: nav bar (back/forward/reload + address entry) over the view.
// Takes the view so popup tabs can adopt a WebKit-created related view.
Gtk::Widget* browser_page(WebKitWebView* view, const std::string& initial_url) {
    webkit_settings_set_user_agent(webkit_web_view_get_settings(view),
                                   kDesktopUserAgent);
    auto* view_widget = wrap_view(view);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    box->set_vexpand(true);

    auto* nav = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    nav->set_margin(4);
    nav->add_css_class("browser-nav");
    auto* back = Gtk::make_managed<Gtk::Button>("◀");
    auto* forward = Gtk::make_managed<Gtk::Button>("▶");
    auto* reload = Gtk::make_managed<Gtk::Button>("⟳");
    for (auto* b : {back, forward, reload}) b->set_has_frame(false);
    back->set_tooltip_text("Back");
    forward->set_tooltip_text("Forward");
    reload->set_tooltip_text("Reload");
    back->set_sensitive(false);
    forward->set_sensitive(false);
    auto* address = Gtk::make_managed<Gtk::Entry>();
    address->set_hexpand(true);
    address->set_width_chars(1);
    address->set_text(initial_url);

    back->signal_clicked().connect([view] { webkit_web_view_go_back(view); });
    forward->signal_clicked().connect([view] { webkit_web_view_go_forward(view); });
    reload->signal_clicked().connect([view] { webkit_web_view_reload(view); });
    address->signal_activate().connect([view, address] {
        const std::string url = HtmlView::normalize_address(address->get_text());
        if (!url.empty()) webkit_web_view_load_uri(view, url.c_str());
    });

    // §6 screen-size presets (📱): constrain the view to a device viewport.
    auto* device_btn = Gtk::make_managed<Gtk::MenuButton>();
    device_btn->set_label("📱");
    device_btn->set_has_frame(false);
    device_btn->set_always_show_arrow(false);
    device_btn->set_tooltip_text("Screen size presets");

    auto* state = new BrowserState{view, address, back, forward, {}, {},
                                   0, 0, nullptr, "", "", false, device_btn};

    {
        auto menu = Gio::Menu::create();
        auto group = Gio::SimpleActionGroup::create();
        const struct {
            const char* name;
            int from, to;
        } sections[] = {
            {"Phones", kPresetPhones, kPresetTablets},
            {"Tablets", kPresetTablets, kPresetLaptops},
            {"Laptops", kPresetLaptops, kPresetDesktops},
            {"Desktops", kPresetDesktops, kPresetCount},
        };
        for (const auto& sec : sections) {
            auto m = Gio::Menu::create();
            for (int i = sec.from; i < sec.to; ++i)
                m->append(kDevicePresets[i].label,
                          "browser_view.device(" + std::to_string(i) + ")");
            menu->append_section(sec.name, m);
        }
        auto actions = Gio::Menu::create();
        actions->append("Rotate (landscape/portrait)", "browser_view.rotate");
        actions->append("Fit to Window", "browser_view.fit");
        menu->append_section(actions);

        group->add_action_with_parameter(
            "device", Glib::VariantType("i"),
            [state](const Glib::VariantBase& param) {
                const int i =
                    Glib::VariantBase::cast_dynamic<Glib::Variant<int>>(param)
                        .get();
                if (i < 0 || i >= kPresetCount) return;
                const auto& d = kDevicePresets[i];
                apply_device(state, d.width, d.height, d.user_agent, d.id,
                             d.label, false);
            });
        group->add_action("rotate", [state] {
            if (state->dev_w > 0)
                apply_device(state, state->dev_h, state->dev_w, state->dev_ua,
                             state->dev_id, state->dev_label,
                             !state->dev_rotated);
        });
        group->add_action("fit", [state] {
            apply_device(state, 0, 0, nullptr, "", "", false);
        });
        device_btn->insert_action_group("browser_view", group);
        device_btn->set_menu_model(menu);
    }
    g_object_set_data_full(G_OBJECT(box->gobj()), kBrowserStateKey, state,
                           [](gpointer p) {
                               delete static_cast<BrowserState*>(p);
                           });
    // The console buffer is looked up from the view (UiControl handlers
    // hold a WebKitWebView*, not the page widget).
    g_object_set_data(G_OBJECT(view), kBrowserStateKey, state);
    track_widget(&state->address, address);
    track_widget(&state->back, back);
    track_widget(&state->forward, forward);
    hook_console(view, state);
    // target="_blank" / window.open(): without a "create" handler WebKit
    // silently drops new-window requests. Create a related view (shares the
    // WebKitWebContext, so window.opener/sessionStorage keep working) and
    // hand it to the editor, which adopts it into a new browser tab.
    // Returning nullptr cancels the navigation (no handler = old behaviour).
    g_signal_connect(view, "create",
                     G_CALLBACK(+[](WebKitWebView* v, WebKitNavigationAction*,
                                    gpointer) -> WebKitWebView* {
                         if (!g_popup_handler) return nullptr;
                         // WebKitGTK 6.0 dropped
                         // webkit_web_view_new_with_related_view(); the
                         // construct-only "related-view" property is the
                         // replacement (shares WebKitWebContext, keeps
                         // window.opener/sessionStorage working).
                         auto* nv = WEBKIT_WEB_VIEW(g_object_new(
                             WEBKIT_TYPE_WEB_VIEW, "related-view", v,
                             nullptr));
                         g_popup_handler(nv);
                         return nv;
                     }),
                     nullptr);
    // §6 browser.upload — a queued upload (UiControl browser.upload)
    // completes the chooser request without a dialog; otherwise return
    // FALSE so WebKit runs its default GTK file chooser.
    g_signal_connect(view, "run-file-chooser",
                     G_CALLBACK(+[](WebKitWebView*,
                                    WebKitFileChooserRequest* request,
                                    gpointer data) -> gboolean {
                         auto* s = static_cast<BrowserState*>(data);
                         if (s->pending_upload.empty()) return FALSE;
                         std::vector<const gchar*> files;
                         files.reserve(s->pending_upload.size() + 1);
                         for (const auto& p : s->pending_upload)
                             files.push_back(p.c_str());
                         files.push_back(nullptr);
                         webkit_file_chooser_request_select_files(
                             request, files.data());
                         s->pending_upload.clear();
                         return TRUE;
                     }),
                     state);
    // Sync the address text + history buttons once a navigation commits;
    // drop the console capture when a new page starts loading.
    g_signal_connect(view, "load-changed",
                     G_CALLBACK(+[](WebKitWebView* v, WebKitLoadEvent event,
                                    gpointer data) {
                         auto* s = static_cast<BrowserState*>(data);
                         if (event == WEBKIT_LOAD_STARTED) {
                             s->console.clear();
                             return;
                         }
                         if (event != WEBKIT_LOAD_COMMITTED) return;
                         // Nav bar may already be finalized (tab closing):
                         // the weak refs above null these out.
                         if (!s->address || !s->back || !s->forward) return;
                         if (const char* uri = webkit_web_view_get_uri(v))
                             s->address->set_text(uri);
                         s->back->set_sensitive(webkit_web_view_can_go_back(v));
                         s->forward->set_sensitive(
                             webkit_web_view_can_go_forward(v));
                     }),
                     state);

    nav->append(*back);
    nav->append(*forward);
    nav->append(*reload);
    nav->append(*address);
    nav->append(*device_btn);
    box->append(*nav);
    // view → clamp (exact sizing, see clamp_measure/allocate) → scroller:
    // small presets center, large presets pan, 0×0 fills the tab (§6).
    auto* clamp = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    // Glib::wrap (take_copy=false) claims the new reference; the temporary
    // RefPtr drops it after set_layout_manager() takes the widget's ref.
    clamp->set_layout_manager(Glib::wrap(
        gtk_custom_layout_new(nullptr, clamp_measure, clamp_allocate)));
    clamp->append(*view_widget);
    auto* view_area = Gtk::make_managed<Gtk::ScrolledWindow>();
    view_area->set_vexpand(true);
    view_area->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    view_area->set_child(*clamp);
    box->append(*view_area);

    if (!initial_url.empty())
        webkit_web_view_load_uri(view, initial_url.c_str());
    return box;
}

} // namespace

Gtk::Widget* HtmlView::create_markdown(const std::string& markdown,
                                       const std::string& base_dir, bool dark) {
    auto* view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    const std::string doc = MarkdownToHtml::to_document(markdown, dark);

    std::string base_uri;
    if (char* uri = g_filename_to_uri(base_dir.c_str(), nullptr, nullptr)) {
        base_uri = uri;
        g_free(uri);
        if (!base_uri.ends_with('/')) base_uri += '/';
    }
    attach_external_link_policy(view);
    webkit_web_view_load_html(view, doc.c_str(),
                              base_uri.empty() ? nullptr : base_uri.c_str());
    return wrap_view(view);
}

Gtk::Widget* HtmlView::create_html_content(const std::string& html,
                                           const std::string& base_dir) {
    auto* view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    std::string base_uri;
    if (char* uri = g_filename_to_uri(base_dir.c_str(), nullptr, nullptr)) {
        base_uri = uri;
        g_free(uri);
        if (!base_uri.ends_with('/')) base_uri += '/';
    }
    // Same link policy as the markdown view: external links leave the app.
    attach_external_link_policy(view);
    webkit_web_view_load_html(view, html.c_str(),
                              base_uri.empty() ? nullptr : base_uri.c_str());
    return wrap_view(view);
}

Gtk::Widget* HtmlView::create_fountain(const std::string& html,
                                       const std::string& base_dir) {
    auto* view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    std::string base_uri;
    if (char* uri = g_filename_to_uri(base_dir.c_str(), nullptr, nullptr)) {
        base_uri = uri;
        g_free(uri);
        if (!base_uri.ends_with('/')) base_uri += '/';
    }
    attach_external_link_policy(view);
    webkit_web_view_load_html(view, html.c_str(),
                              base_uri.empty() ? nullptr : base_uri.c_str());
    return fountain_page(view);
}

double HtmlView::fountain_default_zoom(double dpi, double scale,
                                       double text_scale) {
    if (scale <= 0) scale = 1;
    if (dpi <= 0) dpi = 96.0 * scale; // unknown physical size: standard density
    if (text_scale <= 0) text_scale = 1;
    const double z = std::clamp(dpi / (96.0 * scale) * text_scale, 1.0, 3.0);
    return std::round(z / 0.05) * 0.05;
}

Gtk::Widget* HtmlView::create_html_file(const std::string& path) {
    std::string uri;
    if (char* u = g_filename_to_uri(path.c_str(), nullptr, nullptr)) {
        uri = u;
        g_free(u);
    }
    return browser_page(WEBKIT_WEB_VIEW(webkit_web_view_new()), uri);
}

Gtk::Widget* HtmlView::create_browser(const std::string& initial_url) {
    return browser_page(WEBKIT_WEB_VIEW(webkit_web_view_new()), initial_url);
}

Gtk::Widget* HtmlView::create_browser_adopt(WebKitWebView* view) {
    return browser_page(view, "");
}

void HtmlView::set_popup_handler(std::function<void(WebKitWebView*)> handler) {
    g_popup_handler = std::move(handler);
}

WebKitWebView* HtmlView::browser_view_of(Gtk::Widget* browser_page) {
    if (!browser_page) return nullptr;
    auto* state = static_cast<BrowserState*>(
        g_object_get_data(G_OBJECT(browser_page->gobj()), kBrowserStateKey));
    return state ? state->view : nullptr;
}

std::span<const HtmlView::DevicePreset> HtmlView::device_presets() {
    return kDevicePresets;
}

std::optional<HtmlView::DeviceSize>
HtmlView::device_size(WebKitWebView* view) {
    const auto* state = browser_state(view);
    if (!state) return std::nullopt;
    return DeviceSize{state->dev_w, state->dev_h, state->dev_id,
                      state->dev_label, state->dev_rotated};
}

std::expected<HtmlView::DeviceSize, std::string>
HtmlView::set_device_preset(WebKitWebView* view, std::string_view id_or_label,
                            bool rotate) {
    auto* state = browser_state(view);
    if (!state) return std::unexpected("not a browser view");
    const auto it = std::find_if(
        std::begin(kDevicePresets), std::end(kDevicePresets),
        [id_or_label](const DevicePreset& preset) {
            return id_or_label == preset.id || id_or_label == preset.label;
        });
    if (it == std::end(kDevicePresets))
        return std::unexpected("unknown device preset: " +
                               std::string(id_or_label));
    return apply_device(state, rotate ? it->height : it->width,
                        rotate ? it->width : it->height, it->user_agent,
                        it->id, it->label, rotate);
}

std::expected<HtmlView::DeviceSize, std::string>
HtmlView::rotate_device(WebKitWebView* view) {
    auto* state = browser_state(view);
    if (!state) return std::unexpected("not a browser view");
    if (state->dev_w <= 0)
        return std::unexpected(
            "Fit to Window has no fixed size to rotate; select a preset first");
    return apply_device(state, state->dev_h, state->dev_w, state->dev_ua,
                        state->dev_id, state->dev_label,
                        !state->dev_rotated);
}

std::expected<HtmlView::DeviceSize, std::string>
HtmlView::fit_device_to_window(WebKitWebView* view) {
    auto* state = browser_state(view);
    if (!state) return std::unexpected("not a browser view");
    return apply_device(state, 0, 0, nullptr, "", "", false);
}

std::vector<HtmlView::ConsoleEntry>
HtmlView::console_entries(WebKitWebView* view) {
    std::vector<ConsoleEntry> out;
    if (!view) return out;
    auto* state = static_cast<BrowserState*>(
        g_object_get_data(G_OBJECT(view), kBrowserStateKey));
    if (state)
        out.assign(state->console.begin(), state->console.end());
    return out;
}

void HtmlView::clear_console(WebKitWebView* view) {
    if (!view) return;
    if (auto* state = static_cast<BrowserState*>(
            g_object_get_data(G_OBJECT(view), kBrowserStateKey)))
        state->console.clear();
}

void HtmlView::queue_upload_files(WebKitWebView* view,
                                  const std::vector<std::string>& paths) {
    if (!view) return;
    if (auto* state = static_cast<BrowserState*>(
            g_object_get_data(G_OBJECT(view), kBrowserStateKey)))
        state->pending_upload = paths;
}

std::string HtmlView::normalize_address(const std::string& input) {
    if (input.empty() || input.find("://") != std::string::npos) return input;
    std::error_code ec;
    if (input.front() == '/' && std::filesystem::exists(input, ec)) {
        if (char* uri = g_filename_to_uri(input.c_str(), nullptr, nullptr)) {
            std::string out(uri);
            g_free(uri);
            return out;
        }
    }
    return "http://" + input;
}

} // namespace monkeybyte
