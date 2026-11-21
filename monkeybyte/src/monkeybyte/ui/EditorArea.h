#pragma once

#include "monkeybyte/FileOpenMode.h"

#include <gtkmm.h>
#include <gtksourceview/gtksource.h>
#include <sigc++/sigc++.h>
#include <optional>
#include <string>
#include <vector>

// webkitgtk-6.0 — forward-declared so this header doesn't pull in WebKit.
typedef struct _WebKitWebView WebKitWebView;

namespace monkeybyte {

struct AppServices;

// §6 — Tab bar + file header + single reused GtkSourceView.
class EditorArea : public Gtk::Box {
public:
    explicit EditorArea(AppServices& services);
    ~EditorArea() override;

    void open_file(const std::string& path, int line = 0); // line: 1-based, 0 = no jump
    // Open a read-only viewer tab (Markdown preview); Text defers to open_file.
    void open_file_as(const std::string& path, FileOpenMode mode);
    // Open a URL in a browser tab (§6; agent-openable via browser.open).
    // An empty url opens a blank "New Tab" browser tab — the user types an
    // address into the tab's nav bar (tab-bar ☰ menu → Open Browser).
    void open_url(const std::string& url);

    // Adopt a WebKit-created related view (target="_blank"/window.open
    // popup) into a new browser tab. Called via HtmlView's popup hook.
    void open_browser_view(WebKitWebView* view);

    // Browser-tab access for the browser.* UiControl commands (§6).
    struct BrowserTabInfo {
        std::string url;    // URL the tab was opened with (Tab::path)
        std::string title;  // live page title ("" while still loading)
        bool active = false;
    };
    std::vector<BrowserTabInfo> browser_tabs() const;
    // The active tab's web view when it's a browser tab, else the most
    // recently opened browser tab's; nullptr when no browser tab is open.
    WebKitWebView* active_browser_view();
    // The browser tab whose live URI or initial URL equals `url`; empty
    // `url` falls back to active_browser_view() (design §6 — per-tab
    // targeting for the browser.* control commands).
    WebKitWebView* browser_view_for(const std::string& url);
    // Close the browser tab opened with `url`; empty url closes the tab
    // active_browser_view() would return.
    void close_browser_tab(const std::string& url);
    void goto_line(int line); // 1-based, clamps; selects + centers the line
    bool save_active(); // false = nothing to save

    // Tree context-menu coordination: a workspace file/dir was deleted or
    // renamed outside the editor.
    void close_tabs_under(const std::string& path);
    void retarget_tabs(const std::string& old_path, const std::string& new_path);

    // Tab context-menu actions
    void close_others(size_t index);
    void close_all();
    void close_active();

    sigc::signal<void()> signal_file_deleted() { return signal_file_deleted_; }
    sigc::signal<void(const std::string&)> signal_reveal_file() {
        return signal_reveal_file_;
    }
    sigc::signal<void(const std::string&)> signal_active_file_changed() {
        return signal_active_file_changed_;
    }

    // Current text selection in the active editor tab (UiControl/agent tool).
    struct Selection {
        std::string path;
        std::string text;
        int start_line = 0; // 0-based
        int start_col = 0;  // 0-based char offset within the line
        int end_line = 0;
        int end_col = 0;
    };
    std::optional<Selection> selected_text() const;

private:
    // §6.1 tab state
    struct Tab {
        std::string path;             // absolute
        std::string name;             // basename
        std::string content;          // current buffer (text tabs)
        std::string original_content; // last-saved (text tabs)
        bool is_dirty = false;        // viewer tabs are read-only: never dirty
        bool is_saving = false;
        FileOpenMode kind = FileOpenMode::Text;
        Gtk::Widget* viewer_page = nullptr; // owned by body_stack_, viewer tabs
    };
    static constexpr size_t kNoActive = static_cast<size_t>(-1);

    void rebuild_tab_bar();
    void apply_style_scheme(); // GtkSourceView scheme follows theme (6.2/11)
    void switch_to(size_t index);
    // ☰ tab-bar menu (Add File / Open Browser / …): Gio::Menu + action
    // group wired to tab_menu_btn_; new items are one append + one action.
    void build_tab_actions_menu();
    // Index of the browser tab the browser.* commands act on: the active
    // tab if it's a browser, else the most recently opened one; kNoActive
    // when no browser tab is open.
    size_t browser_tab_index() const;
    void close_tab(size_t index, bool skip_dirty_check = false);
    void show_tab_menu(Gtk::Widget& tab_widget, size_t index, double x, double y);
    void show_edit_menu(double x, double y);
    void delete_active();
    void new_file();
    void apply_editor_settings();
    void sync_from_buffer();
    void show_placeholder();
    bool confirm_discard(const std::string& name, std::function<void()> on_confirm);
    void update_header();
    Gtk::Widget* build_viewer_page(FileOpenMode kind, const std::string& path,
                                   const std::string& content);
    // Re-read the active Markdown preview from disk without closing its tab.
    void refresh_active_preview();

    // find / replace bar
    void build_search_bar();
    void show_search(bool with_replace);
    void find_next(bool forward);

    AppServices& services_;

    std::vector<Tab> tabs_;
    size_t active_ = kNoActive;

    // widgets
    Gtk::Box tab_strip_;
    Gtk::ScrolledWindow tab_scroller_;
    Gtk::MenuButton tab_menu_btn_; // ☰ dropdown (build_tab_actions_menu)
    Gtk::Label placeholder_;
    Gtk::Box header_;           // file header (§6.3)
    Gtk::Label header_name_;
    Gtk::Label header_status_;
    Gtk::Button refresh_btn_, save_btn_, delete_btn_, close_btn_;
    Gtk::ScrolledWindow view_scroller_;
    Gtk::Stack body_stack_; // "text" page = view_scroller_; one page per viewer tab

    GtkWidget* source_view_ = nullptr;   // GtkSourceView (C API)
    GtkSourceBuffer* source_buffer_ = nullptr;
    GtkSourceSearchContext* search_context_ = nullptr;
    GtkSourceSearchSettings* search_settings_ = nullptr;
    Glib::RefPtr<Gtk::CssProvider> font_css_; // single reused font provider

    bool loading_ = false; // guard buffer-changed during tab switches

    // search bar
    Gtk::Box search_bar_;
    Gtk::SearchEntry search_entry_;
    Gtk::Entry replace_entry_;
    Gtk::Button replace_btn_, replace_all_btn_;

    sigc::signal<void()> signal_file_deleted_;
    sigc::signal<void(const std::string&)> signal_reveal_file_;
    sigc::signal<void(const std::string&)> signal_active_file_changed_;
};

} // namespace monkeybyte
