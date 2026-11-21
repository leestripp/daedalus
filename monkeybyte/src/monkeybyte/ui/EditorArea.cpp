#include "monkeybyte/ui/EditorArea.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/FountainTools.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/ui/FileIcons.h"
#include "monkeybyte/ui/HtmlView.h"
#include "monkeybyte/ui/UiGuard.h"
#include "monkeybyte/ui/dialogs/NewFileDialog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>

namespace monkeybyte {

namespace {

// §6.2 — CodeMirror-mode → GtkSourceLanguage map.
const std::map<std::string, std::string>& language_map() {
    static const std::map<std::string, std::string> map = {
        {"js", "javascript"}, {"mjs", "javascript"}, {"jsx", "javascript"},
        {"ts", "typescript"}, {"tsx", "typescript"},
        {"py", "python"},
        {"cpp", "cpp"}, {"cc", "cpp"}, {"cxx", "cpp"}, {"hpp", "cpp"},
        {"h", "c"}, {"c", "c"},
        {"java", "java"}, {"go", "go"}, {"rs", "rust"},
        {"html", "html"}, {"htm", "html"},
        {"css", "css"}, {"scss", "css"}, {"sass", "css"}, {"less", "css"},
        {"xml", "xml"},
        {"md", "markdown"}, {"markdown", "markdown"},
        {"json", "json"},
        {"yaml", "yaml"}, {"yml", "yaml"},
        {"sh", "sh"}, {"bash", "sh"}, {"zsh", "sh"}, {"fish", "sh"},
        {"sql", "sql"},
    };
    return map;
}

std::string extension_of(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

EditorArea::EditorArea(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL),
      services_(services),
      tab_strip_(Gtk::Orientation::HORIZONTAL, 0),
      header_(Gtk::Orientation::HORIZONTAL, 8),
      search_bar_(Gtk::Orientation::HORIZONTAL, 6) {
    // Popup hook: browser tabs' target="_blank"/window.open() requests
    // arrive here with a related view to adopt (HtmlView "create" signal).
    HtmlView::set_popup_handler(
        [this](WebKitWebView* v) { open_browser_view(v); });
    // ── tab bar (§6.1): scrolling strip + ☰ actions menu ─────────────
    auto* tab_bar = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    tab_bar->add_css_class("tab-bar");
    tab_scroller_.set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::NEVER);
    tab_scroller_.set_hexpand(true);
    tab_scroller_.set_child(tab_strip_);
    tab_bar->append(tab_scroller_);
    tab_menu_btn_.set_label("☰");
    tab_menu_btn_.set_has_frame(false);
    tab_menu_btn_.set_tooltip_text("Editor actions");
    build_tab_actions_menu();
    tab_bar->append(tab_menu_btn_);
    append(*tab_bar);

    // ── file header (§6.3) ───────────────────────────────────────────
    header_.add_css_class("file-header");
    header_name_.set_halign(Gtk::Align::START);
    header_name_.set_hexpand(true);
    // Browser tabs use the full URL as their name. An unbroken URL must not
    // become the editor's natural width and push the browser over the
    // sidebar; the tooltip still exposes the complete address.
    header_name_.set_width_chars(1);
    header_name_.set_max_width_chars(60);
    header_name_.set_ellipsize(Pango::EllipsizeMode::END);
    header_status_.add_css_class("muted");
    refresh_btn_.set_label("↻ Refresh");
    refresh_btn_.set_has_frame(false);
    refresh_btn_.set_tooltip_text("Reload Markdown and images from disk");
    refresh_btn_.signal_clicked().connect([this] {
        ui_guard("refresh_markdown_preview", [this] { refresh_active_preview(); });
    });
    save_btn_.set_label("💾 Save");
    save_btn_.set_has_frame(false);
    save_btn_.signal_clicked().connect([this] { save_active(); });
    delete_btn_.set_has_frame(false);
    {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name("mb-trash-symbolic");
        icon->set_pixel_size(14);
        box->append(*icon);
        box->append(*Gtk::make_managed<Gtk::Label>("Delete"));
        delete_btn_.set_child(*box);
    }
    delete_btn_.signal_clicked().connect([this] { delete_active(); });
    close_btn_.set_label("× Close");
    close_btn_.set_has_frame(false);
    close_btn_.signal_clicked().connect([this] { close_tab(active_); });
    header_.append(header_name_);
    header_.append(header_status_);
    header_.append(refresh_btn_);
    header_.append(save_btn_);
    header_.append(delete_btn_);
    header_.append(close_btn_);
    append(header_);

    // ── GtkSourceView (single reused instance, §6.2) ─────────────────
    source_view_ = gtk_source_view_new();
    source_buffer_ = GTK_SOURCE_BUFFER(gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(source_view_)));
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(source_view_), TRUE);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(source_view_), TRUE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(source_view_), TRUE);

    // ── find/replace bar (§6.2, §9) — needs source_buffer_ ───────────
    build_search_bar();
    append(search_bar_);

    auto* view = Glib::wrap(source_view_);
    view->set_vexpand(true);
    view_scroller_.set_child(*view);
    view_scroller_.set_vexpand(true);
    // Body is a stack: the shared source view is the "text" page; viewer
    // tabs (read-only previews) each get their own page (§6 preview tabs).
    body_stack_.add(view_scroller_, "text");
    body_stack_.set_vexpand(true);
    append(body_stack_);

    // buffer edits → recompute dirty
    g_signal_connect(source_buffer_, "changed",
                     G_CALLBACK(+[](GtkTextBuffer*, gpointer self) {
                         static_cast<EditorArea*>(self)->sync_from_buffer();
                     }),
                     this);

    // auto-close brackets (§6.2: GtkEventControllerKey)
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType state) {
            static const std::map<guint, const char*> pairs = {
                {GDK_KEY_parenleft, "()"}, {GDK_KEY_bracketleft, "[]"},
                {GDK_KEY_braceleft, "{}"}, {GDK_KEY_quotedbl, "\"\""},
                {GDK_KEY_apostrophe, "''"},
            };
            auto it = pairs.find(keyval);
            if (it == pairs.end()) return false;
            auto* buffer = GTK_TEXT_BUFFER(source_buffer_);
            gtk_text_buffer_begin_user_action(buffer);
            gtk_text_buffer_insert_at_cursor(buffer, it->second, 2);
            GtkTextIter iter;
            gtk_text_buffer_get_iter_at_mark(
                buffer, &iter, gtk_text_buffer_get_insert(buffer));
            gtk_text_iter_backward_char(&iter);
            gtk_text_buffer_place_cursor(buffer, &iter);
            gtk_text_buffer_end_user_action(buffer);
            (void)state;
            return true;
        },
        false);
    view->add_controller(keys);

    // right-click edit menu (GTK4 text views have no built-in one)
    auto edit_click = Gtk::GestureClick::create();
    edit_click->set_button(GDK_BUTTON_SECONDARY);
    edit_click->signal_pressed().connect(
        [this](int, double x, double y) { show_edit_menu(x, y); });
    view->add_controller(edit_click);

    // Ctrl+F / Ctrl+H / Ctrl+G / Shift+Ctrl+G (§9)
    auto shortcuts = Gtk::ShortcutController::create();
    auto add_shortcut = [&](const char* trigger, std::function<void()> fn) {
        auto action = Gtk::CallbackAction::create(
            [fn = std::move(fn)](Gtk::Widget&, const Glib::VariantBase&) {
                fn();
                return true;
            });
        shortcuts->add_shortcut(Gtk::Shortcut::create(
            Gtk::ShortcutTrigger::parse_string(trigger), action));
    };
    add_shortcut("<Control>f", [this] { show_search(false); });
    add_shortcut("<Control>h", [this] { show_search(true); });
    add_shortcut("<Control>g", [this] { find_next(true); });
    add_shortcut("<Shift><Control>g", [this] { find_next(false); });
    add_controller(shortcuts);

    // style scheme: Adwaita-dark / Adwaita (§6.2)
    // (design 6.2) - follows theme switches live via signal_applied (11).
    apply_style_scheme();
    services_.theme.signal_applied().connect(
        [this] { apply_style_scheme(); });

    // Live-apply editor settings changed from the Settings panel (§4.5).
    for (const char* key : {"font-size", "tab-size", "word-wrap",
                            "line-numbers", "highlight-current-line"})
        services_.editor->signal_changed(key).connect(
            [this](const Glib::ustring&) { apply_editor_settings(); });

    apply_editor_settings();
    show_placeholder();
}

EditorArea::~EditorArea() = default;

void EditorArea::apply_style_scheme() {
    auto* schemes = gtk_source_style_scheme_manager_get_default();
    const bool dark = services_.theme.current() != "light";
    if (auto* scheme = gtk_source_style_scheme_manager_get_scheme(
            schemes, dark ? "Adwaita-dark" : "Adwaita"))
        gtk_source_buffer_set_style_scheme(source_buffer_, scheme);
}

// ── placeholder / visibility ────────────────────────────────────────
void EditorArea::show_placeholder() {
    if (tabs_.empty()) {
        placeholder_.set_text("Open a file from the Workspace.");
        placeholder_.add_css_class("editor-empty");
        placeholder_.add_css_class("muted");
        if (!placeholder_.get_parent()) {
            placeholder_.set_halign(Gtk::Align::FILL);
            placeholder_.set_valign(Gtk::Align::FILL);
            placeholder_.set_hexpand(true);
            placeholder_.set_vexpand(true);
            append(placeholder_);
        }
        placeholder_.set_visible(true);
        body_stack_.set_visible(false);
        header_.set_visible(false);
        search_bar_.set_visible(false);
    } else {
        placeholder_.set_visible(false);
        body_stack_.set_visible(true);
        header_.set_visible(true);
    }
}

// ── tab bar ─────────────────────────────────────────────────────────
void EditorArea::rebuild_tab_bar() {
    while (auto* child = tab_strip_.get_first_child())
        tab_strip_.remove(*child);

    for (size_t i = 0; i < tabs_.size(); ++i) {
        const auto& tab = tabs_[i];
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
        box->add_css_class("tab");
        if (i == active_) box->add_css_class("active");

        auto* icon = Gtk::make_managed<Gtk::Image>();
        icon->set_from_icon_name(tab.kind == FileOpenMode::Browser
                                     ? "mb-globe-symbolic"
                                     : file_icon_name(tab.name, false));
        icon->set_pixel_size(14);
        auto* name = Gtk::make_managed<Gtk::Label>(tab.name);
        name->set_max_width_chars(15);
        name->set_ellipsize(Pango::EllipsizeMode::END);
        box->append(*icon);
        box->append(*name);
        if (tab.is_dirty) {
            auto* dot = Gtk::make_managed<Gtk::Label>("●");
            dot->add_css_class("accent");
            box->append(*dot);
        }
        auto* close = Gtk::make_managed<Gtk::Button>("×");
        close->set_has_frame(false);
        close->signal_clicked().connect([this, i] { close_tab(i); });
        box->append(*close);

        auto click = Gtk::GestureClick::create();
        click->set_button(GDK_BUTTON_PRIMARY);
        click->signal_pressed().connect(
            [this, box, close, i](int, double x, double y) {
                // Ignore presses on the close button: switch_to() rebuilds
                // the strip, which would destroy the button before release,
                // so its "clicked" would never fire (tab could not close).
                if (auto* picked = box->pick(x, y, Gtk::PickFlags::DEFAULT)) {
                    for (auto* w = picked; w; w = w->get_parent())
                        if (w == close) return;
                }
                switch_to(i);
            });
        box->add_controller(click);

        // right-click tab menu (§6.1)
        auto menu_click = Gtk::GestureClick::create();
        menu_click->set_button(GDK_BUTTON_SECONDARY);
        menu_click->signal_pressed().connect(
            [this, box, i](int, double x, double y) {
                show_tab_menu(*box, i, x, y);
            });
        box->add_controller(menu_click);

        tab_strip_.append(*box);
    }
    update_header();
}

void EditorArea::update_header() {
    if (active_ == kNoActive || active_ >= tabs_.size()) {
        header_name_.set_text("");
        header_name_.set_tooltip_text("");
        header_status_.set_text("");
        refresh_btn_.set_visible(false);
        save_btn_.set_sensitive(false);
        delete_btn_.set_sensitive(false);
        return;
    }
    const auto& tab = tabs_[active_];
    header_name_.set_text(tab.name + (tab.is_dirty ? " ●" : ""));
    header_name_.set_tooltip_text(tab.name);
    // Viewer tabs are read-only: Save never applies. Browser tabs hold a
    // URL, not a workspace file: Delete does not apply either.
    refresh_btn_.set_visible(tab.kind == FileOpenMode::MarkdownPreview);
    save_btn_.set_sensitive(tab.kind == FileOpenMode::Text && tab.is_dirty &&
                            !tab.is_saving);
    delete_btn_.set_sensitive(tab.kind != FileOpenMode::Browser);
}

// ── open / switch / close ───────────────────────────────────────────
void EditorArea::open_file(const std::string& path, int line) {
    // focus existing tab (text tabs only; a preview tab on the same file is
    // a separate tab keyed by path+kind)
    const auto existing = std::find_if(tabs_.begin(), tabs_.end(),
                                       [&](const Tab& t) {
                                           return t.path == path &&
                                                  t.kind == FileOpenMode::Text;
                                       });
    if (existing != tabs_.end()) {
        switch_to(static_cast<size_t>(std::distance(tabs_.begin(), existing)));
        if (line > 0) goto_line(line);
        return;
    }
    auto content = services_.files.read(path);
    if (!content) {
        header_status_.set_text("Error: " + content.error());
        return;
    }
    // Binary guard: any caller (Git panel, search, …) can route a non-text
    // file here. Refuse instead of rendering garbage into the text buffer.
    if (content->find('\0', 0) != std::string::npos) {
        header_status_.set_text(
            "Binary file not opened — use Open with System Application");
        return;
    }
    Tab tab;
    tab.path = path;
    tab.name = std::filesystem::path(path).filename().string();
    tab.content = *content;
    tab.original_content = *content;
    tabs_.push_back(std::move(tab));
    switch_to(tabs_.size() - 1);
    if (line > 0) goto_line(line);
}

void EditorArea::refresh_active_preview() {
    if (active_ == kNoActive || active_ >= tabs_.size()) return;
    Tab& tab = tabs_[active_];
    if (tab.kind != FileOpenMode::MarkdownPreview || !tab.viewer_page) return;

    auto content = services_.files.read(tab.path);
    if (!content) {
        header_status_.set_text("Error: " + content.error());
        return;
    }
    const bool dark = services_.theme.current() != "light";
    const std::string base_dir =
        std::filesystem::path(tab.path).parent_path().string();
    if (HtmlView::reload_markdown(*tab.viewer_page, *content, base_dir, dark))
        header_status_.set_text("");
}

void EditorArea::open_file_as(const std::string& path, FileOpenMode mode) {
    if (mode == FileOpenMode::Text) {
        open_file(path);
        return;
    }
    const auto existing = std::find_if(tabs_.begin(), tabs_.end(),
                                       [&](const Tab& t) {
                                           return t.path == path && t.kind == mode;
                                       });
    if (existing != tabs_.end()) {
        switch_to(static_cast<size_t>(std::distance(tabs_.begin(), existing)));
        return;
    }
    auto content = services_.files.read(path);
    if (!content) {
        header_status_.set_text("Error: " + content.error());
        return;
    }
    Tab tab;
    tab.path = path;
    tab.name = std::filesystem::path(path).filename().string();
    tab.kind = mode;
    tab.viewer_page = build_viewer_page(mode, path, *content);
    body_stack_.add(*tab.viewer_page);
    tabs_.push_back(std::move(tab));
    switch_to(tabs_.size() - 1);
}

// Read-only WebKit viewer page (§6 preview tabs). Markdown refreshes also
// reapply the current theme palette.
Gtk::Widget* EditorArea::build_viewer_page(FileOpenMode kind,
                                           const std::string& path,
                                           const std::string& content) {
    const bool dark = services_.theme.current() != "light";
    const std::string base_dir =
        std::filesystem::path(path).parent_path().string();
    if (kind == FileOpenMode::MarkdownPreview)
        return HtmlView::create_markdown(content, base_dir, dark);
    if (kind == FileOpenMode::FountainPreview) {
        // Plan.md P4: rendered by an external converter (wrap/screenplain).
        // Without one, show the install hint instead of a broken page.
        std::string html = fountain::to_html(content);
        if (html.empty())
            html = "<html><body style='font-family:sans-serif;padding:2em'>"
                   "<h2>Fountain preview unavailable</h2><p>" +
                   std::string(fountain::kInstallHint) + "</p></body></html>";
        return HtmlView::create_fountain(html, base_dir);
    }
    return HtmlView::create_html_file(path); // FileOpenMode::HtmlPreview
}

void EditorArea::open_url(const std::string& url) {
    // Browser tabs store the URL in Tab::path. Coordination with workspace
    // files (close_tabs_under / retarget_tabs) is unaffected: those match
    // absolute paths, and a URL never starts with a workspace path prefix.
    // An empty url is a blank "New Tab" — the nav bar's address entry is
    // the source of truth from there (☰ menu → Open Browser).
    const auto existing = std::find_if(tabs_.begin(), tabs_.end(),
                                       [&](const Tab& t) {
                                           return t.path == url &&
                                                  t.kind == FileOpenMode::Browser;
                                       });
    if (existing != tabs_.end()) {
        switch_to(static_cast<size_t>(std::distance(tabs_.begin(), existing)));
        return;
    }
    Tab tab;
    tab.path = url;
    tab.name = url.empty() ? "New Tab" : url;
    tab.kind = FileOpenMode::Browser;
    tab.viewer_page = HtmlView::create_browser(url);
    body_stack_.add(*tab.viewer_page);
    tabs_.push_back(std::move(tab));
    switch_to(tabs_.size() - 1);
}

void EditorArea::open_browser_view(WebKitWebView* view) {
    // Popup tab: the URL is unknown until WebKit drives the load into the
    // adopted view (load-changed then fills the nav-bar address entry).
    Tab tab;
    tab.path.clear();
    tab.name = "New Tab";
    tab.kind = FileOpenMode::Browser;
    tab.viewer_page = HtmlView::create_browser_adopt(view);
    body_stack_.add(*tab.viewer_page);
    tabs_.push_back(std::move(tab));
    switch_to(tabs_.size() - 1);
}

size_t EditorArea::browser_tab_index() const {
    if (active_ != kNoActive && active_ < tabs_.size() &&
        tabs_[active_].kind == FileOpenMode::Browser)
        return active_;
    for (size_t i = tabs_.size(); i-- > 0;)
        if (tabs_[i].kind == FileOpenMode::Browser) return i;
    return kNoActive;
}

std::vector<EditorArea::BrowserTabInfo> EditorArea::browser_tabs() const {
    std::vector<BrowserTabInfo> out;
    for (size_t i = 0; i < tabs_.size(); ++i) {
        const Tab& t = tabs_[i];
        if (t.kind != FileOpenMode::Browser) continue;
        BrowserTabInfo info;
        info.url = t.path;
        info.active = (i == active_);
        if (auto* view = HtmlView::browser_view_of(t.viewer_page)) {
            if (const char* title = webkit_web_view_get_title(view))
                info.title = title;
        }
        out.push_back(std::move(info));
    }
    return out;
}

WebKitWebView* EditorArea::active_browser_view() {
    const size_t i = browser_tab_index();
    return i == kNoActive ? nullptr
                          : HtmlView::browser_view_of(tabs_[i].viewer_page);
}

WebKitWebView* EditorArea::browser_view_for(const std::string& url) {
    if (url.empty()) return active_browser_view();
    for (const Tab& t : tabs_) {
        if (t.kind != FileOpenMode::Browser) continue;
        auto* view = HtmlView::browser_view_of(t.viewer_page);
        if (!view) continue;
        const char* live = webkit_web_view_get_uri(view);
        if (t.path == url || (live && url == live)) return view;
    }
    return nullptr;
}

void EditorArea::close_browser_tab(const std::string& url) {
    size_t i = kNoActive;
    if (url.empty()) {
        i = browser_tab_index();
    } else {
        const auto it = std::find_if(tabs_.begin(), tabs_.end(),
                                     [&](const Tab& t) {
                                         return t.kind == FileOpenMode::Browser &&
                                                t.path == url;
                                     });
        if (it != tabs_.end())
            i = static_cast<size_t>(std::distance(tabs_.begin(), it));
    }
    // Browser tabs are never dirty; no confirm dialog needed.
    if (i != kNoActive) close_tab(i, /*skip_dirty_check=*/true);
}

// §4.3 — search-result jump: select the line (also moves the cursor, so the
// current-line highlight kicks in) and center it in the viewport.
void EditorArea::goto_line(int line) {
    auto* buffer = GTK_TEXT_BUFFER(source_buffer_);
    line = std::clamp(line, 1, gtk_text_buffer_get_line_count(buffer));
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_line(buffer, &start, line - 1);
    GtkTextIter end = start;
    gtk_text_iter_forward_to_line_end(&end);
    gtk_text_buffer_select_range(buffer, &start, &end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(source_view_), &start, 0.0,
                                 TRUE, 1.0, 0.5);
}

void EditorArea::switch_to(size_t index) {
    if (index >= tabs_.size()) return;
    active_ = index;
    const auto& tab = tabs_[index];

    if (tab.kind == FileOpenMode::Text) {
        loading_ = true;
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(source_buffer_),
                                 tab.content.c_str(),
                                 static_cast<gint>(tab.content.size()));
        loading_ = false;

        // language detection (§6.2)
        const std::string ext = extension_of(tab.path);
        GtkSourceLanguage* lang = nullptr;
        if (auto it = language_map().find(ext); it != language_map().end())
            lang = gtk_source_language_manager_get_language(
                gtk_source_language_manager_get_default(), it->second.c_str());
        gtk_source_buffer_set_language(source_buffer_, lang);

        apply_editor_settings(); // settings applied on every activation (§6.2)
        body_stack_.set_visible_child("text");
    } else if (tab.viewer_page) {
        // Viewer tab: leave the shared source buffer untouched.
        body_stack_.set_visible_child(*tab.viewer_page);
    }

    rebuild_tab_bar();
    show_placeholder();
    // Browser tabs hold a URL, not a workspace file: nothing to reveal.
    if (tab.kind != FileOpenMode::Browser)
        signal_active_file_changed_.emit(tab.path);
}

void EditorArea::close_tab(size_t index, bool skip_dirty_check) {
    if (index >= tabs_.size()) return;
    if (tabs_[index].is_dirty && !skip_dirty_check) {
        const std::string name = tabs_[index].name;
        const std::string path = tabs_[index].path;
        const FileOpenMode kind = tabs_[index].kind;
        // Re-resolve by path+kind on confirm: batch closes (Close Others/All)
        // can shift indices while the dialog is open, and the same file can
        // be open as both a text and a preview tab.
        confirm_discard(name, [this, path, kind] {
            const auto it = std::find_if(tabs_.begin(), tabs_.end(),
                                         [&](const Tab& t) {
                                             return t.path == path && t.kind == kind;
                                         });
            if (it != tabs_.end())
                close_tab(static_cast<size_t>(std::distance(tabs_.begin(), it)),
                          /*skip_dirty_check=*/true);
        });
        return;
    }
    const bool was_active = (index == active_);
    if (tabs_[index].viewer_page)
        body_stack_.remove(*tabs_[index].viewer_page); // destroys the page
    tabs_.erase(tabs_.begin() + static_cast<long>(index));

    if (tabs_.empty()) {
        active_ = kNoActive;
        loading_ = true;
        gtk_text_buffer_set_text(GTK_TEXT_BUFFER(source_buffer_), "", 0);
        loading_ = false;
    } else if (was_active) {
        // previous index, or the new last (§6.1)
        active_ = index > 0 ? index - 1 : 0;
        if (active_ >= tabs_.size()) active_ = tabs_.size() - 1;
        switch_to(active_);
        return;
    } else if (active_ > index) {
        --active_;
    }
    rebuild_tab_bar();
    show_placeholder();
}

bool EditorArea::confirm_discard(const std::string& name,
                                 std::function<void()> on_confirm) {
    auto* dialog = gtk_alert_dialog_new("\"%s\" has unsaved changes.", name.c_str());
    gtk_alert_dialog_set_detail(GTK_ALERT_DIALOG(dialog),
                                "Close without saving?");
    const char* buttons[] = {"Cancel", "Close Without Saving", nullptr};
    gtk_alert_dialog_set_buttons(GTK_ALERT_DIALOG(dialog), buttons);
    gtk_alert_dialog_set_default_button(GTK_ALERT_DIALOG(dialog), 0);
    gtk_alert_dialog_set_cancel_button(GTK_ALERT_DIALOG(dialog), 0);
    auto* on_confirm_ptr = new std::function<void()>(std::move(on_confirm));
    gtk_alert_dialog_choose(
        GTK_ALERT_DIALOG(dialog),
        [] (Gtk::Widget& w) -> GtkWindow* {
            // wrapper -> C object (see ChatPanel approval dialog note)
            auto* rw = dynamic_cast<Gtk::Window*>(w.get_root());
            return rw ? GTK_WINDOW(rw->gobj()) : nullptr;
        }(*this), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::function<void()>> fn(
                static_cast<std::function<void()>*>(data));
            const int choice = gtk_alert_dialog_choose_finish(
                GTK_ALERT_DIALOG(source), result, nullptr);
            if (choice == 1) (*fn)();
        },
        on_confirm_ptr);
    g_object_unref(dialog);
    return true;
}

// ── buffer sync / save / delete ─────────────────────────────────────
void EditorArea::sync_from_buffer() {
    if (loading_ || active_ == kNoActive || active_ >= tabs_.size()) return;
    if (tabs_[active_].kind != FileOpenMode::Text) return; // viewer tab: no buffer
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(source_buffer_), &start, &end);
    char* text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(source_buffer_),
                                          &start, &end, FALSE);
    Tab& tab = tabs_[active_];
    tab.content = text;
    g_free(text);
    const bool dirty = (tab.content != tab.original_content);
    if (dirty != tab.is_dirty) {
        tab.is_dirty = dirty;
        rebuild_tab_bar();
    }
    update_header();
}

std::optional<EditorArea::Selection> EditorArea::selected_text() const {
    if (active_ == kNoActive || active_ >= tabs_.size()) return std::nullopt;
    if (tabs_[active_].kind != FileOpenMode::Text) return std::nullopt;
    auto* buffer = GTK_TEXT_BUFFER(source_buffer_);
    if (!gtk_text_buffer_get_has_selection(buffer)) return std::nullopt;
    GtkTextIter start, end;
    gtk_text_buffer_get_selection_bounds(buffer, &start, &end);
    char* text = gtk_text_buffer_get_text(buffer, &start, &end, TRUE);
    Selection sel;
    sel.path = tabs_[active_].path;
    sel.text = text ? text : "";
    g_free(text);
    sel.start_line = gtk_text_iter_get_line(&start);
    sel.start_col = gtk_text_iter_get_line_offset(&start);
    sel.end_line = gtk_text_iter_get_line(&end);
    sel.end_col = gtk_text_iter_get_line_offset(&end);
    return sel;
}

bool EditorArea::save_active() {
    if (active_ == kNoActive || active_ >= tabs_.size()) return false;
    Tab& tab = tabs_[active_];
    if (!tab.is_dirty || tab.is_saving) return false;

    tab.is_saving = true;
    header_status_.set_text("Saving…");
    auto result = services_.files.write(tab.path, tab.content);
    tab.is_saving = false;

    if (!result) {
        header_status_.set_text("Error: " + result.error());
        update_header();
        return false;
    }
    tab.original_content = tab.content;
    tab.is_dirty = false;
    header_status_.set_text("Saved");
    Glib::signal_timeout().connect_once(
        [this] {
            if (header_status_.get_text() == "Saved") header_status_.set_text("");
        },
        2000);
    rebuild_tab_bar();
    return true;
}

void EditorArea::delete_active() {
    if (active_ == kNoActive || active_ >= tabs_.size()) return;
    const std::string path = tabs_[active_].path;
    const std::string name = tabs_[active_].name;

    auto* dialog = gtk_alert_dialog_new("Delete \"%s\"?", name.c_str());
    gtk_alert_dialog_set_detail(GTK_ALERT_DIALOG(dialog),
                                tabs_[active_].is_dirty
                                    ? "This file has unsaved changes. Delete it permanently?"
                                    : "This file will be permanently deleted.");
    const char* buttons[] = {"Cancel", "Delete", nullptr};
    gtk_alert_dialog_set_buttons(GTK_ALERT_DIALOG(dialog), buttons);
    gtk_alert_dialog_set_cancel_button(GTK_ALERT_DIALOG(dialog), 0);
    gtk_alert_dialog_set_default_button(GTK_ALERT_DIALOG(dialog), 0);

    struct Ctx {
        EditorArea* self;
        std::string path;
    };
    auto* ctx = new Ctx{this, path};
    gtk_alert_dialog_choose(
        GTK_ALERT_DIALOG(dialog),
        [] (Gtk::Widget& w) -> GtkWindow* {
            // wrapper -> C object (see ChatPanel approval dialog note)
            auto* rw = dynamic_cast<Gtk::Window*>(w.get_root());
            return rw ? GTK_WINDOW(rw->gobj()) : nullptr;
        }(*this), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Ctx> ctx(static_cast<Ctx*>(data));
            const int choice = gtk_alert_dialog_choose_finish(
                GTK_ALERT_DIALOG(source), result, nullptr);
            if (choice == 1) {
                if (auto r = ctx->self->services_.files.remove(ctx->path)) {
                    // Re-resolve by path: tabs can shift while the dialog is
                    // open, so a captured index could close the WRONG tab
                    // with skip_dirty_check=true (same hazard close_tab's
                    // dirty confirm guards against).
                    ctx->self->close_tabs_under(ctx->path);
                    ctx->self->signal_file_deleted_.emit();
                } else {
                    Logger::instance().error("delete failed: " + r.error());
                }
            }
        },
        ctx);
    g_object_unref(dialog);
}

void EditorArea::new_file() {
    auto* root = dynamic_cast<Gtk::Window*>(get_root());
    auto* dlg = Gtk::make_managed<NewFileDialog>(
        root ? *root : *Gtk::make_managed<Gtk::Window>(), services_);
    dlg->signal_file_created().connect(
        [this](const std::string& path) { open_file(path); });
    dlg->present();
}

// ☰ tab-bar dropdown. Same Gio::Menu + SimpleActionGroup pattern as the
// tab/edit context menus; add future items with one append + one action.
void EditorArea::build_tab_actions_menu() {
    auto menu = Gio::Menu::create();
    auto group = Gio::SimpleActionGroup::create();
    menu->append("Add File", "editor_menu.new_file");
    group->add_action("new_file", [this] { new_file(); });
    menu->append("Open Browser", "editor_menu.open_browser");
    group->add_action("open_browser", [this] {
        // Settings → Interface → Browser home URL (§4.5); empty = blank
        // "New Tab". Normalized like the nav bar's address entry so a bare
        // host ("localhost:3000") works too.
        ui_guard("open_browser", [this] {
            open_url(HtmlView::normalize_address(
                services_.store.load_interface().browser_home_url));
        });
    });

    tab_menu_btn_.insert_action_group("editor_menu", group);
    tab_menu_btn_.set_menu_model(menu);
}

// ── tree context-menu coordination ──────────────────────────────────
void EditorArea::close_tabs_under(const std::string& path) {
    // `path` is a deleted file or directory; close any tab on it or inside
    // it (no dirty confirm — the file is gone). Iterate backwards so
    // close_tab()'s erase keeps not-yet-visited indices valid.
    for (size_t i = tabs_.size(); i-- > 0;) {
        const auto& p = tabs_[i].path;
        if (p == path || p.starts_with(path + "/"))
            close_tab(i, /*skip_dirty_check=*/true);
    }
}

// ── tab / edit context menus ────────────────────────────────────────
void EditorArea::close_others(size_t index) {
    if (index >= tabs_.size()) return;
    // Close tabs after `index` first (keeps `index` valid), then before it.
    for (size_t i = tabs_.size(); i-- > index + 1;) close_tab(i);
    for (size_t i = index; i-- > 0;) close_tab(i);
}

void EditorArea::close_all() {
    for (size_t i = tabs_.size(); i-- > 0;) close_tab(i);
}

void EditorArea::close_active() {
    if (active_ != kNoActive && active_ < tabs_.size()) close_tab(active_);
}

void EditorArea::show_tab_menu(Gtk::Widget& tab_widget, size_t index,
                               double x, double y) {
    if (index >= tabs_.size()) return;
    const std::string path = tabs_[index].path;

    auto menu = Gio::Menu::create();
    auto group = Gio::SimpleActionGroup::create();
    menu->append("Close", "tab.close");
    group->add_action("close", [this, index] { close_tab(index); });
    menu->append("Close Others", "tab.close_others");
    group->add_action("close_others", [this, index] { close_others(index); });
    menu->append("Close All", "tab.close_all");
    group->add_action("close_all", [this] { close_all(); });
    menu->append("Reveal in Explorer", "tab.reveal");
    group->add_action("reveal",
                      [this, path] { signal_reveal_file_.emit(path); });
    menu->append("Copy Path", "tab.copy_path");
    group->add_action("copy_path", [&tab_widget, path] {
        tab_widget.get_clipboard()->set_text(path);
    });

    tab_widget.insert_action_group("tab", group);
    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_parent(tab_widget);
    popover->set_has_arrow(false);
    popover->set_pointing_to(Gdk::Rectangle(
        static_cast<int>(x), static_cast<int>(y), 1, 1));
    popover->popup();
}

void EditorArea::show_edit_menu(double x, double y) {
    auto* view = Glib::wrap(source_view_);
    auto* buffer = GTK_TEXT_BUFFER(source_buffer_);

    auto menu = Gio::Menu::create();
    auto group = Gio::SimpleActionGroup::create();
    menu->append("Cut", "edit.cut");
    group->add_action("cut", [buffer, view] {
        gtk_text_buffer_cut_clipboard(
            buffer, gtk_widget_get_clipboard(GTK_WIDGET(view->gobj())),
            gtk_text_view_get_editable(GTK_TEXT_VIEW(view->gobj())));
    });
    menu->append("Copy", "edit.copy");
    group->add_action("copy", [buffer, view] {
        gtk_text_buffer_copy_clipboard(
            buffer, gtk_widget_get_clipboard(GTK_WIDGET(view->gobj())));
    });
    menu->append("Paste", "edit.paste");
    group->add_action("paste", [buffer, view] {
        gtk_text_buffer_paste_clipboard(
            buffer, gtk_widget_get_clipboard(GTK_WIDGET(view->gobj())),
            nullptr,
            gtk_text_view_get_editable(GTK_TEXT_VIEW(view->gobj())));
    });
    menu->append("Select All", "edit.select_all");
    group->add_action("select_all", [buffer] {
        GtkTextIter start, end;
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gtk_text_buffer_select_range(buffer, &start, &end);
    });

    view->insert_action_group("edit", group);
    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_parent(*view);
    popover->set_has_arrow(false);
    popover->set_pointing_to(Gdk::Rectangle(
        static_cast<int>(x), static_cast<int>(y), 1, 1));
    popover->popup();
}

void EditorArea::retarget_tabs(const std::string& old_path,
                               const std::string& new_path) {
    bool changed = false;
    for (auto& tab : tabs_) {
        if (tab.path == old_path) {
            tab.path = new_path;
            tab.name = std::filesystem::path(new_path).filename().string();
            changed = true;
        } else if (tab.path.starts_with(old_path + "/")) {
            tab.path = new_path + tab.path.substr(old_path.size());
            tab.name = std::filesystem::path(tab.path).filename().string();
            changed = true;
        }
    }
    if (changed) rebuild_tab_bar(); // also updates the file header
}

// ── settings ────────────────────────────────────────────────────────
void EditorArea::apply_editor_settings() {
    ui_guard("apply_editor_settings", [this] {
    // SQLite is authoritative (§10.2); GSettings keys are only the change
    // notification channel.
    const auto s = services_.store.load_interface();

    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(source_view_),
                                  static_cast<guint>(s.tab_size));
    gtk_source_view_set_insert_spaces_instead_of_tabs(
        GTK_SOURCE_VIEW(source_view_), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(source_view_),
                                s.word_wrap ? GTK_WRAP_WORD : GTK_WRAP_NONE);
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(source_view_),
                                          s.show_line_numbers);
    gtk_source_view_set_highlight_current_line(
        GTK_SOURCE_VIEW(source_view_), s.highlight_current_line);

    auto* view = Glib::wrap(source_view_);
    if (!font_css_) {
        font_css_ = Gtk::CssProvider::create();
        view->get_style_context()->add_provider(
            font_css_, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    font_css_->load_from_string("textview { font-family: monospace; font-size: " +
                                std::to_string(s.font_size) + "pt; }");
    });
}

// ── search / replace (§6.2, §9) ─────────────────────────────────────
void EditorArea::build_search_bar() {
    search_bar_.set_margin(4);
    search_entry_.set_hexpand(true);
    search_entry_.set_placeholder_text("Find…");
    replace_entry_.set_placeholder_text("Replace…");
    replace_entry_.set_hexpand(true);
    replace_btn_.set_label("Replace");
    replace_all_btn_.set_label("All");

    auto* prev = Gtk::make_managed<Gtk::Button>("↑");
    prev->set_tooltip_text("Previous (Shift+Ctrl+G)");
    prev->signal_clicked().connect([this] { find_next(false); });
    auto* next = Gtk::make_managed<Gtk::Button>("↓");
    next->set_tooltip_text("Next (Ctrl+G)");
    next->signal_clicked().connect([this] { find_next(true); });
    auto* close = Gtk::make_managed<Gtk::Button>("×");
    close->signal_clicked().connect([this] { search_bar_.set_visible(false); });

    search_bar_.append(search_entry_);
    search_bar_.append(replace_entry_);
    search_bar_.append(replace_btn_);
    search_bar_.append(replace_all_btn_);
    search_bar_.append(*prev);
    search_bar_.append(*next);
    search_bar_.append(*close);
    search_bar_.set_visible(false);

    search_settings_ = gtk_source_search_settings_new();
    gtk_source_search_settings_set_case_sensitive(search_settings_, FALSE);
    search_context_ = gtk_source_search_context_new(source_buffer_, search_settings_);
    gtk_source_search_context_set_highlight(search_context_, TRUE);

    search_entry_.signal_search_changed().connect([this] {
        gtk_source_search_settings_set_search_text(
            search_settings_, search_entry_.get_text().c_str());
    });
    search_entry_.signal_activate().connect([this] { find_next(true); });

    replace_btn_.signal_clicked().connect([this] {
        GtkTextIter start, end;
        auto* buffer = GTK_TEXT_BUFFER(source_buffer_);
        if (gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
            gtk_source_search_context_replace(
                search_context_, &start, &end,
                replace_entry_.get_text().c_str(), -1, nullptr);
        }
        find_next(true);
    });
    replace_all_btn_.signal_clicked().connect([this] {
        gtk_source_search_context_replace_all(
            search_context_, replace_entry_.get_text().c_str(), -1, nullptr);
        sync_from_buffer();
    });

    // Escape closes the bar and returns focus to the editor.
    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) {
                search_bar_.set_visible(false);
                gtk_widget_grab_focus(source_view_);
                return true;
            }
            return false;
        },
        false);
    search_bar_.add_controller(keys);
}

void EditorArea::show_search(bool with_replace) {
    if (active_ == kNoActive || active_ >= tabs_.size()) return;
    // The search bar is bound to the single source buffer (§9): text tabs only.
    if (tabs_[active_].kind != FileOpenMode::Text) return;
    search_bar_.set_visible(true);
    replace_entry_.set_visible(with_replace);
    replace_btn_.set_visible(with_replace);
    replace_all_btn_.set_visible(with_replace);
    search_entry_.grab_focus();
    search_entry_.select_region(0, -1);
}

void EditorArea::find_next(bool forward) {
    if (active_ == kNoActive || !search_context_) return;
    auto* buffer = GTK_TEXT_BUFFER(source_buffer_);
    GtkTextIter iter, match_start, match_end;
    gtk_text_buffer_get_iter_at_mark(buffer, &iter,
                                     gtk_text_buffer_get_insert(buffer));
    gboolean found = forward
        ? gtk_source_search_context_forward(search_context_, &iter,
                                            &match_start, &match_end, nullptr)
        : gtk_source_search_context_backward(search_context_, &iter,
                                             &match_start, &match_end, nullptr);
    if (!found) {
        // wrap around
        if (forward)
            gtk_text_buffer_get_start_iter(buffer, &iter);
        else
            gtk_text_buffer_get_end_iter(buffer, &iter);
        found = forward
            ? gtk_source_search_context_forward(search_context_, &iter,
                                                &match_start, &match_end, nullptr)
            : gtk_source_search_context_backward(search_context_, &iter,
                                                 &match_start, &match_end, nullptr);
    }
    if (found) {
        gtk_text_buffer_select_range(buffer, &match_start, &match_end);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(source_view_), &match_start,
                                     0.0, FALSE, 0.0, 0.0);
    }
}

} // namespace monkeybyte
