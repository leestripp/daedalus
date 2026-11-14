#pragma once

#include <gtkmm.h>
#include <functional>
#include <string>
#include <vector>
#include <webkit/webkit.h>

namespace monkeybyte {

// §6 preview tabs — WebKitGTK-backed read-only HTML viewer for the editor
// area. Markdown mode converts markdown → HTML (backend/MarkdownToHtml)
// and loads it with a file:// base URI so relative images resolve; HTML and
// browser modes add a navigation bar (back/forward/reload + address entry)
// above the web view. The chat's Pango markdown renderer (§7.4) is
// untouched.
class HtmlView {
public:
    // Rendered markdown document. base_dir = the .md file's directory, used
    // as the base URI for relative image/link paths. dark selects the
    // embedded Mocha/Latte stylesheet. Bare document view, no nav bar.
    static Gtk::Widget* create_markdown(const std::string& markdown,
                                        const std::string& base_dir, bool dark);

    // Rendered HTML document string (e.g. a Fountain screenplay converted
    // by an external tool, Plan.md P4). Bare document view, no nav bar;
    // http(s) links open in the system browser like the markdown view.
    static Gtk::Widget* create_html_content(const std::string& html,
                                            const std::string& base_dir);

    // Fountain screenplay preview (Plan.md P4): the create_html_content
    // document view plus a zoom bar (- / percentage / +) and Ctrl +/- /0
    // shortcuts. Converter HTML is styled for print (12 pt Courier on a
    // paper-sized page), so the initial zoom is display-aware - derived
    // from the monitor's physical DPI vs. its scale factor plus the
    // desktop text-scaling factor - keeping text readable on high-DPI
    // screens. The chosen zoom is shared by all Fountain previews for the
    // session.
    static Gtk::Widget* create_fountain(const std::string& html,
                                        const std::string& base_dir);

    // Display-aware default zoom used by create_fountain (pure, exposed
    // for unit tests): dpi / (96 * scale) * text_scale - WebKit lays out
    // at a logical 96 DPI and the compositor already applies the monitor
    // scale factor on top, so a result above 1 means print-styled text
    // renders physically small (typical: 4K at 100% desktop scaling).
    // Clamped to [1.0, 3.0] and snapped to 5% steps; dpi <= 0 (unknown
    // physical size) assumes standard density, scale/text_scale <= 0 -> 1.
    static double fountain_default_zoom(double dpi, double scale,
                                        double text_scale);

    // Local HTML file with a nav bar; the address entry is pre-filled with
    // the file:// URI and can be edited to navigate elsewhere.
    static Gtk::Widget* create_html_file(const std::string& path);

    // Browser tab adopting an already-created WebKitWebView (popups:
    // target="_blank" / window.open() - WebKit's "create" signal hands us
    // a related view, wrapped here in the usual nav-bar page).
    static Gtk::Widget* create_browser_adopt(WebKitWebView* view);

    // Installed once by EditorArea: called from a browser tab's "create"
    // signal with a new related view to adopt into a new tab. With no
    // handler installed, new-window requests are dropped (WebKit default).
    static void set_popup_handler(std::function<void(WebKitWebView*)> handler);

    // Browser tab (§6): nav bar + web view loading an arbitrary URL, e.g. a
    // local dev server the agent just started (opened via browser.open).
    static Gtk::Widget* create_browser(const std::string& initial_url);

    // The WebKitWebView inside a widget returned by create_browser /
    // create_html_file (nullptr for markdown views). Lets UiControl
    // commands (browser.read/navigate/list) drive an open browser tab.
    static WebKitWebView* browser_view_of(Gtk::Widget* browser_page);

    // A captured console/page-error entry (design §6 browser.console).
    struct ConsoleEntry {
        std::string level; // "error" | "warn" | "log"
        std::string text;
    };

    // Console/error entries captured for a browser tab's view (newest
    // last, bounded). Empty for markdown views. Captured by an injected
    // user script wrapping console.* / window error events.
    static std::vector<ConsoleEntry> console_entries(WebKitWebView* view);
    // Empty the capture buffer (browser.console {clear:true}).
    static void clear_console(WebKitWebView* view);

    // §6 browser.upload — queue local file paths that the next
    // run-file-chooser request in this view completes with (consumed on
    // first use). With no queue, WebKit's default GTK file chooser runs.
    static void queue_upload_files(WebKitWebView* view,
                                   const std::vector<std::string>& paths);

    // Address-entry normalization shared by the nav bar and the
    // browser.navigate control command: full URIs pass through, an existing
    // absolute file path becomes file://, anything else gets http://.
    static std::string normalize_address(const std::string& input);
};

} // namespace monkeybyte
