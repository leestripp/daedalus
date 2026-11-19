#include "monkeybyte/ui/panels/SettingsPanel.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/ChimpFlowClient.h"
#include "monkeybyte/backend/ProjectTypes.h"
#include "monkeybyte/backend/McpBridge.h"
#include "monkeybyte/backend/ModelBridge.h"
#include "monkeybyte/backend/SkillDiscovery.h"
#include "monkeybyte/ui/dialogs/McpJsonImportDialog.h"
#include "monkeybyte/ui/dialogs/McpServerDialog.h"
#include "monkeybyte/ui/dialogs/ModelEditDialog.h"
#include "monkeybyte/ui/dialogs/ProviderEditDialog.h"
#include "monkeybyte/ui/dialogs/SystemPromptDialog.h"
#include "monkeybyte/ui/dialogs/WorkspacePickerDialog.h"
#include "monkeybyte/ui/UiGuard.h"

#include <nlohmann/json.hpp>
#include <daedalus/ProviderPlugin.h>
#include <daedalus/ProviderRegistry.h>
#include <daedalus/default_paths.h>
#include <daedalus/model_discovery.h>
#include <daedalus/paths.h>
#include <daedalus/settings.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace monkeybyte {

namespace {

Gtk::Window* toplevel_of(Gtk::Widget& w) {
    return dynamic_cast<Gtk::Window*>(w.get_root());
}

// SettingsStore throws std::runtime_error on any SQLite failure. The
// settings panel auto-saves on every control change, so a DB hiccup would
// otherwise kill the app straight out of a GTK signal handler — log and
// degrade instead (§7.2).
// NOTE: returns nullopt when the load fails. Callers that auto-save MUST
// bail on nullopt -- saving a default-constructed struct after a failed
// load would clobber every other interface setting (this is how a SQLite
// busy hiccup once wiped browser_home_url).
std::optional<InterfaceSettings> load_interface_guarded(const SettingsStore& store) {
    InterfaceSettings s;
    bool ok = false;
    ui_guard("settings load_interface",
             [&] { s = store.load_interface(); ok = true; });
    if (!ok) return std::nullopt;
    return s;
}

void save_interface_guarded(SettingsStore& store, const InterfaceSettings& s) {
    ui_guard("settings save_interface", [&] { store.save_interface(s); });
}

Gtk::Box* group_header(const Glib::ustring& text) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
    box->add_css_class("settings-section-header");
    auto* label = Gtk::make_managed<Gtk::Label>(text);
    label->set_halign(Gtk::Align::START);
    label->add_css_class("settings-section-title");
    auto* sep = Gtk::make_managed<Gtk::Separator>();
    sep->add_css_class("settings-section-rule");
    box->append(*label);
    box->append(*sep);
    return box;
}

Gtk::Box* field_row(const Glib::ustring& label, Gtk::Widget& control) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 20);
    row->add_css_class("settings-field");
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->set_halign(Gtk::Align::START);
    l->set_size_request(220, -1);
    l->set_wrap(true);
    row->append(*l);

    const bool fill_width = dynamic_cast<Gtk::Entry*>(&control) ||
                            dynamic_cast<Gtk::PasswordEntry*>(&control) ||
                            dynamic_cast<Gtk::DropDown*>(&control) ||
                            dynamic_cast<Gtk::SpinButton*>(&control);
    control.set_hexpand(fill_width);
    control.set_halign(fill_width ? Gtk::Align::FILL : Gtk::Align::END);
    row->append(control);
    return row;
}

class SettingsScroll final : public Gtk::ScrolledWindow {
public:
    void set_content(Gtk::Box& page) {
        page_ = &page;
        add_css_class("settings-scroll");
        page.add_css_class("settings-page");
        page.set_halign(Gtk::Align::CENTER);
        page.set_hexpand(false);
        set_child(page);
    }

protected:
    void size_allocate_vfunc(int width, int height, int baseline) override {
        if (page_) {
            constexpr int max_content_width = 900;
            constexpr int horizontal_gutter = 48;
            const int target = std::max(0, std::min(max_content_width,
                                                    width - horizontal_gutter));
            int requested_width = 0;
            int requested_height = 0;
            page_->get_size_request(requested_width, requested_height);
            if (requested_width != target) page_->set_size_request(target, -1);
        }
        Gtk::ScrolledWindow::size_allocate_vfunc(width, height, baseline);
    }

private:
    Gtk::Box* page_ = nullptr;
};

void finish_settings_page(SettingsScroll& scroll, Gtk::Box& page) {
    scroll.set_content(page);
}

std::string selected_dropdown_text(const Gtk::DropDown& dropdown) {
    if (auto item = dropdown.get_selected_item()) {
        if (auto text = std::dynamic_pointer_cast<const Gtk::StringObject>(item))
            return text->get_string();
    }
    return {};
}

void replace_dropdown_items(Gtk::DropDown& dropdown,
                            Glib::RefPtr<Gtk::StringList>& model,
                            std::vector<std::string> items,
                            const std::string& preferred,
                            bool& updating) {
    if (!preferred.empty() &&
        std::find(items.begin(), items.end(), preferred) == items.end()) {
        items.push_back(preferred);
    }
    updating = true;
    model = Gtk::StringList::create({});
    unsigned selected = GTK_INVALID_LIST_POSITION;
    for (const auto& item : items) {
        model->append(item);
        if (item == preferred) selected = model->get_n_items() - 1;
    }
    dropdown.set_model(model);
    if (selected != GTK_INVALID_LIST_POSITION) dropdown.set_selected(selected);
    else if (model->get_n_items() > 0) dropdown.set_selected(0);
    updating = false;
}

// Provider leading icon (§4.5).
std::string provider_icon(const std::string& provider) {
    static const std::unordered_map<std::string, std::string> icons = {
        {"minimax", "🔷"}, {"openai", "⚡"}, {"claude", "🟣"}, {"ollama", "🦙"},
        {"gemini", "✨"}, {"mistral", "🌬️"}, {"grok", "🚀"}, {"kimi", "🌙"},
    };
    std::string lower;
    for (char c : provider) lower += static_cast<char>(std::tolower(c));
    auto it = icons.find(lower);
    return it != icons.end() ? it->second : "🤖";
}

// Confirmation dialog for destructive settings actions (mirrors
// EditorArea::confirm_discard).
void confirm_delete(Gtk::Window* parent, const std::string& title,
                    std::function<void()> on_confirm) {
    auto* dialog = gtk_alert_dialog_new("%s", title.c_str());
    gtk_alert_dialog_set_detail(GTK_ALERT_DIALOG(dialog),
                                "This cannot be undone.");
    const char* buttons[] = {"Cancel", "Delete", nullptr};
    gtk_alert_dialog_set_buttons(GTK_ALERT_DIALOG(dialog), buttons);
    gtk_alert_dialog_set_cancel_button(GTK_ALERT_DIALOG(dialog), 0);
    gtk_alert_dialog_set_default_button(GTK_ALERT_DIALOG(dialog), 0);
    auto* on_confirm_ptr = new std::function<void()>(std::move(on_confirm));
    gtk_alert_dialog_choose(
        GTK_ALERT_DIALOG(dialog), parent ? GTK_WINDOW(parent->gobj()) : nullptr,
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<std::function<void()>> fn(
                static_cast<std::function<void()>*>(data));
            const int choice = gtk_alert_dialog_choose_finish(
                GTK_ALERT_DIALOG(source), result, nullptr);
            if (choice == 1) (*fn)();
        },
        on_confirm_ptr);
    g_object_unref(dialog);
}

// §4.5 — "Fetch Models" picker: lists models discovered at a provider's
// /v1/models endpoint; checked rows are added as UiModels on confirm.
void show_model_picker(Gtk::Window* parent, const std::string& provider_name,
                       daedalus::ModelDiscoveryResult result,
                       std::function<void()> on_added) {
    auto* dlg = Gtk::make_managed<Gtk::Window>();
    dlg->set_title("Models at " + provider_name);
    dlg->set_transient_for(*parent);
    dlg->set_modal(true);
    dlg->set_default_size(420, 360);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);
    dlg->set_child(*body);

    if (!result.success) {
        auto* err = Gtk::make_managed<Gtk::Label>(
            "Discovery failed: " + result.error_message);
        err->set_wrap(true);
        err->set_halign(Gtk::Align::START);
        body->append(*err);
    } else if (result.models.empty()) {
        auto* none = Gtk::make_managed<Gtk::Label>("No models reported.");
        none->set_halign(Gtk::Align::START);
        body->append(*none);
    }

    std::vector<std::pair<Gtk::CheckButton*, std::string>> checks;
    if (result.success && !result.models.empty()) {
        auto* list_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
        list_scroll->set_vexpand(true);
        auto* list = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        for (const auto& dm : result.models) {
            std::string label = dm.id;
            if (dm.context_length > 0)
                label += "  (" + std::to_string(dm.context_length / 1024) + "k ctx)";
            auto* check = Gtk::make_managed<Gtk::CheckButton>(label);
            check->set_active(true);
            check->set_halign(Gtk::Align::START);
            list->append(*check);
            checks.emplace_back(check, dm.id);
        }
        list_scroll->set_child(*list);
        body->append(*list_scroll);
    }

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([dlg] { dlg->hide(); });
    footer->append(*cancel);
    if (result.success && !result.models.empty()) {
        auto* add = Gtk::make_managed<Gtk::Button>("Add Selected");
        add->add_css_class("suggested-action");
        add->signal_clicked().connect(
            [dlg, provider_name, checks, on_added = std::move(on_added)] {
                ui_guard("add discovered models", [&] {
                    auto& registry = daedalus::SettingsStore::instance();
                    for (const auto& [check, id] : checks) {
                        if (!check->get_active()) continue;
                        UiModel m;
                        m.name = id;
                        m.provider = provider_name;
                        m.model_id = id;
                        model_bridge::save_model(registry, m);
                    }
                });
                on_added();
                dlg->hide();
            });
        footer->append(*add);
    }
    body->append(*footer);
    dlg->present();
}

// Runs provider model discovery off the UI thread, then opens the picker.
void fetch_models_async(Gtk::Window* parent, UiProvider p,
                        std::function<void()> on_added) {
    std::thread([parent, p = std::move(p), on_added = std::move(on_added)] {
        // Detached worker: any escape is std::terminate — discovery does
        // HTTP + JSON, so catch everything and report via the picker.
        daedalus::ModelDiscoveryResult result;
        ui_guard("fetch_models", [&] {
            const std::string key = model_bridge::resolve_env_key(p.api_key);
            result = daedalus::discover_models(p.host, key, p.discovery_endpoint);
        });
        Glib::signal_idle().connect_once(
            [parent, p = std::move(p), result = std::move(result),
             on_added = std::move(on_added)]() mutable {
                show_model_picker(parent, p.name, std::move(result),
                                  std::move(on_added));
            });
    }).detach();
}

} // namespace

SettingsPanel::~SettingsPanel() {
    *alive_ = false;
    ++chimpflow_discovery_generation_;
    ++chimpflow_preview_generation_;
}

SettingsPanel::SettingsPanel(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL), services_(services) {
    add_css_class("settings-panel");
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    header->add_css_class("settings-header");
    auto* title = Gtk::make_managed<Gtk::Label>("Settings");
    title->set_halign(Gtk::Align::START);
    title->add_css_class("settings-title");
    auto* subtitle = Gtk::make_managed<Gtk::Label>(
        "Configure your workspace, agent, and integrations");
    subtitle->set_halign(Gtk::Align::START);
    subtitle->add_css_class("settings-subtitle");
    header->append(*title);
    header->append(*subtitle);
    append(*header);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    body->set_vexpand(true);
    auto* tabs = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    tabs->add_css_class("settings-nav");
    tabs->set_size_request(184, -1);
    const char* names[] = {"Interface", "Providers", "Models", "Workspace",
                           "MCP Servers", "Agent", "Web Search", "ChimpFlow",
                           "Search", "Paths"};
    stack_ = Gtk::make_managed<Gtk::Stack>();
    const char* ids[] = {"interface", "providers", "models", "workspace", "mcp",
                         "agent", "websearch", "chimpflow", "search", "paths"};
    for (int i = 0; i < 10; ++i) {
        auto* b = Gtk::make_managed<Gtk::Button>(names[i]);
        b->set_has_frame(false);
        b->add_css_class("settings-nav-button");
        b->set_halign(Gtk::Align::FILL);
        if (auto* child = b->get_child()) child->set_halign(Gtk::Align::START);
        b->signal_clicked().connect([this, i, id = ids[i]] {
            stack_->set_visible_child(id);
            if (std::string_view(id) == "chimpflow") refresh_chimpflow_catalog();
            for (size_t j = 0; j < tab_buttons_.size(); ++j) {
                if (static_cast<int>(j) == i)
                    tab_buttons_[j]->add_css_class("active");
                else
                    tab_buttons_[j]->remove_css_class("active");
            }
        });
        tab_buttons_.push_back(b);
        tabs->append(*b);
    }
    tab_buttons_[0]->add_css_class("active");
    body->append(*tabs);
    body->append(*Gtk::make_managed<Gtk::Separator>(Gtk::Orientation::VERTICAL));

    stack_->set_vexpand(true);
    stack_->set_hexpand(true);
    stack_->add(*build_interface_tab(), "interface");
    stack_->add(*build_providers_tab(), "providers");
    stack_->add(*build_models_tab(), "models");
    stack_->add(*build_workspace_tab(), "workspace");
    stack_->add(*build_mcp_tab(), "mcp");
    stack_->add(*build_agent_tab(), "agent");
    stack_->add(*build_web_search_tab(), "websearch");
    stack_->add(*build_chimpflow_tab(), "chimpflow");
    stack_->add(*build_search_tab(), "search");
    stack_->add(*build_paths_tab(), "paths");
    body->append(*stack_);
    append(*body);

    // ── footer ───────────────────────────────────────────────────────
    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->add_css_class("settings-footer");
    footer->set_margin(8);
    status_label_.set_halign(Gtk::Align::START);
    status_label_.set_hexpand(true);
    status_label_.add_css_class("muted");
    auto* save = Gtk::make_managed<Gtk::Button>("Save Settings");
    save->add_css_class("suggested-action");
    save->signal_clicked().connect([this] { on_save(); });
    footer->append(status_label_);
    footer->append(*save);
    append(*footer);
}

// ── Tab 1: Interface (§4.5) ─────────────────────────────────────────
Gtk::Widget* SettingsPanel::build_interface_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(12);

    // Read-only initial widget state: defaults are safe here (nothing is
    // saved on construct).
    const auto current =
        load_interface_guarded(services_.store).value_or(InterfaceSettings{});

    auto persist = [this] { status_label_.set_text(""); };

    // Workspace group (design §12.1: Settings is a workspace-change entry point)
    box->append(*group_header("WORKSPACE"));
    workspace_label_ = Gtk::make_managed<Gtk::Label>();
    workspace_label_->set_halign(Gtk::Align::START);
    workspace_label_->set_hexpand(true);
    workspace_label_->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    auto update_workspace_label = [this] {
        const auto& ws = services_.files.workspace();
        workspace_label_->set_text(ws.empty() ? "No workspace selected"
                                              : ws.string());
    };
    update_workspace_label();
    // Workspace can change while Settings is hidden; sync when shown again.
    signal_map().connect([update_workspace_label] { update_workspace_label(); });
    auto* change_btn = Gtk::make_managed<Gtk::Button>("Change…");
    change_btn->signal_clicked().connect([this, update_workspace_label] {
        auto* dlg = Gtk::make_managed<WorkspacePickerDialog>(
            *toplevel_of(*this), services_);
        dlg->signal_folder_chosen().connect(
            [this, update_workspace_label](const std::string& path) {
                update_workspace_label();
                signal_workspace_changed_.emit(path);
            });
        dlg->present();
    });
    {
        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row->set_margin_start(8);
        row->append(*workspace_label_);
        row->append(*change_btn);
        box->append(*row);
    }

    // Editor group
    box->append(*group_header("EDITOR"));

    auto* font_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"12", "13", "14", "15", "16", "17", "18", "19", "20"}));
    font_dd->set_selected(static_cast<unsigned>(current.font_size - 12));
    font_dd->property_selected().signal_changed().connect([this, font_dd, persist] {
        const int v = 12 + static_cast<int>(font_dd->get_selected());
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->font_size = v;
        save_interface_guarded(services_.store, *s);
        services_.editor->set_int("font-size", v); // live apply (§4.5)
        persist();
    });
    box->append(*field_row("Font Size", *font_dd));

    auto* tab_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"2 spaces", "4 spaces", "8 spaces"}));
    const unsigned tab_idx = current.tab_size == 4 ? 1 : (current.tab_size == 8 ? 2 : 0);
    tab_dd->set_selected(tab_idx);
    tab_dd->property_selected().signal_changed().connect([this, tab_dd, persist] {
        const int v = 2 << static_cast<int>(tab_dd->get_selected()); // 2/4/8
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->tab_size = v;
        save_interface_guarded(services_.store, *s);
        services_.editor->set_int("tab-size", v);
        persist();
    });
    box->append(*field_row("Tab Size", *tab_dd));

    auto* wrap_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Off", "On"}));
    wrap_dd->set_selected(current.word_wrap ? 1 : 0);
    wrap_dd->property_selected().signal_changed().connect([this, wrap_dd, persist] {
        const bool v = wrap_dd->get_selected() == 1;
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->word_wrap = v;
        save_interface_guarded(services_.store, *s);
        services_.editor->set_boolean("word-wrap", v);
        persist();
    });
    box->append(*field_row("Word Wrap", *wrap_dd));

    auto* lines_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Off", "On"}));
    lines_dd->set_selected(current.show_line_numbers ? 1 : 0);
    lines_dd->property_selected().signal_changed().connect([this, lines_dd, persist] {
        const bool v = lines_dd->get_selected() == 1;
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->show_line_numbers = v;
        save_interface_guarded(services_.store, *s);
        services_.editor->set_boolean("line-numbers", v);
        persist();
    });
    box->append(*field_row("Line Numbers", *lines_dd));

    auto* hl_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Off", "On"}));
    hl_dd->set_selected(current.highlight_current_line ? 1 : 0);
    hl_dd->property_selected().signal_changed().connect([this, hl_dd, persist] {
        const bool v = hl_dd->get_selected() == 1;
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->highlight_current_line = v;
        save_interface_guarded(services_.store, *s);
        services_.editor->set_boolean("highlight-current-line", v);
        persist();
    });
    box->append(*field_row("Highlight Current Line", *hl_dd));

    // Theme group
    box->append(*group_header("THEME"));
    auto* theme_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"System", "Dark", "Light"}));
    theme_dd->set_selected(current.theme == "dark"   ? 1
                           : current.theme == "light" ? 2
                                                       : 0);
    theme_dd->property_selected().signal_changed().connect([this, theme_dd] {
        static const char* values[] = {"system", "dark", "light"};
        const std::string v = values[theme_dd->get_selected()];
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->theme = v;
        save_interface_guarded(services_.store, *s);
        services_.theme_settings->set_string("name", v);
        services_.theme.apply(v); // §12.4: apply immediately
        signal_theme_changed_.emit();
    });
    box->append(*field_row("Color Theme", *theme_dd));

    // Explorer group
    box->append(*group_header("EXPLORER"));
    auto* hidden_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Off", "On"}));
    hidden_dd->set_selected(current.show_hidden ? 1 : 0);
    hidden_dd->property_selected().signal_changed().connect([this, hidden_dd, persist] {
        const bool v = hidden_dd->get_selected() == 1;
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->show_hidden = v;
        save_interface_guarded(services_.store, *s);
        services_.files.set_show_hidden(v); // live apply
        signal_tree_refresh_.emit();
        persist();
    });
    box->append(*field_row("Show Hidden Files", *hidden_dd));

    // Browser group (§6 browser tabs)
    box->append(*group_header("BROWSER"));
    auto* home_entry = Gtk::make_managed<Gtk::Entry>();
    home_entry->set_text(current.browser_home_url);
    home_entry->set_placeholder_text("https://… (empty = blank tab)");
    home_entry->set_width_chars(28);
    home_entry->signal_changed().connect([this, home_entry, persist] {
        auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
        s->browser_home_url = home_entry->get_text();
        save_interface_guarded(services_.store, *s);
        persist();
    });
    box->append(*field_row("Home URL", *home_entry));

    // Chat group (§4.5) — appearance overrides layered above the theme
    // (ThemeManager::set_chat_overrides). Empty/0 = theme default, so the
    // untouched look follows the active dark/light theme.
    box->append(*group_header("CHAT"));
    auto apply_chat = [this] {
        if (const auto cur = load_interface_guarded(services_.store))
            services_.theme.set_chat_overrides(*cur);
    };

    auto* chat_font_dd = Gtk::make_managed<Gtk::DropDown>(
        Gtk::StringList::create({"Default", "10 px", "11 px", "12 px", "13 px",
                                 "14 px", "15 px", "16 px", "17 px", "18 px"}));
    chat_font_dd->set_selected(
        current.chat_font_size >= 10 && current.chat_font_size <= 18
            ? static_cast<unsigned>(current.chat_font_size - 9)
            : 0);
    chat_font_dd->property_selected().signal_changed().connect(
        [this, chat_font_dd, apply_chat, persist] {
            const auto sel = chat_font_dd->get_selected();
            auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
            s->chat_font_size = sel == 0 ? 0 : static_cast<int>(sel + 9);
            save_interface_guarded(services_.store, *s);
            apply_chat();
            persist();
        });
    box->append(*field_row("Bubble Font Size", *chat_font_dd));

    auto to_hex = [](const Gdk::RGBA& c) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "#%02x%02x%02x",
                      static_cast<int>(c.get_red() * 255 + 0.5),
                      static_cast<int>(c.get_green() * 255 + 0.5),
                      static_cast<int>(c.get_blue() * 255 + 0.5));
        return std::string(buf);
    };

    // Default swatches mirror the theme CSS so the button always shows the
    // color currently in effect (override if set, else the theme value).
    struct ChatColorRow {
        const char* label;
        const char* dark_default;
        const char* light_default;
        std::string InterfaceSettings::* member;
    };
    // static: color_btns below keeps pointers into this table for the
    // lifetime of the panel's signal handlers.
    static const ChatColorRow chat_colors[] = {
        {"Chat Background", "#181825", "#e6e9ef", &InterfaceSettings::chat_bg},
        {"User Bubble Background", "#89b4fa", "#1e66f5", &InterfaceSettings::chat_user_bg},
        {"User Text", "#11111b", "#eff1f5", &InterfaceSettings::chat_user_text},
        {"Assistant Bubble Background", "#313244", "#ccd0da", &InterfaceSettings::chat_assistant_bg},
        {"Assistant Text", "#cdd6f4", "#4c4f69", &InterfaceSettings::chat_assistant_text},
    };

    auto color_dialog = Gtk::ColorDialog::create();
    color_dialog->set_with_alpha(false);
    // Reset re-points the buttons at the theme defaults; suppress the
    // rgba-changed handler while doing so or it re-stores them as overrides.
    auto resetting = std::make_shared<bool>(false);
    // Fallback swatch for a row follows the *resolved* theme at query time.
    auto fallback_for = [](const ChatColorRow& cc, const std::string& resolved) {
        return resolved == "light" ? cc.light_default : cc.dark_default;
    };
    struct ChatColorBtn {
        Gtk::ColorDialogButton* btn;
        const ChatColorRow* row;
    };
    std::vector<ChatColorBtn> color_btns;
    for (const auto& cc : chat_colors) {
        auto* btn = Gtk::make_managed<Gtk::ColorDialogButton>(color_dialog);
        const std::string fallback = fallback_for(cc, services_.theme.current());
        Gdk::RGBA rgba;
        rgba.set((current.*(cc.member)).empty() ? fallback : current.*(cc.member));
        btn->set_rgba(rgba);
        btn->property_rgba().signal_changed().connect(
            [this, btn, member = cc.member, to_hex, apply_chat, persist, resetting] {
                if (*resetting) return;
                auto s = load_interface_guarded(services_.store);
                if (!s) return; // DB hiccup: don't clobber settings with defaults
                (*s).*member = to_hex(btn->get_rgba());
                save_interface_guarded(services_.store, *s);
                apply_chat();
                persist();
            });
        color_btns.push_back({btn, &cc});
        box->append(*field_row(cc.label, *btn));
    }

    // Theme switch: re-point swatches with no stored override at the new
    // theme's default (connected after the theme_dd handler above, so
    // theme.current() is already updated when this runs).
    theme_dd->property_selected().signal_changed().connect(
        [this, color_btns, fallback_for, resetting] {
            const auto s = load_interface_guarded(services_.store);
            if (!s) return;
            *resetting = true;
            for (const auto& [btn, row] : color_btns) {
                if (!((*s).*(row->member)).empty()) continue; // explicit override stays
                Gdk::RGBA rgba;
                rgba.set(fallback_for(*row, services_.theme.current()));
                btn->set_rgba(rgba);
            }
            *resetting = false;
        });

    auto* reset_btn = Gtk::make_managed<Gtk::Button>("Reset Chat Appearance");
    reset_btn->set_halign(Gtk::Align::START);
    reset_btn->set_margin_start(8);
    reset_btn->signal_clicked().connect(
        [this, chat_font_dd, color_btns, apply_chat, persist, resetting, fallback_for] {
            auto s = load_interface_guarded(services_.store);
        if (!s) return; // DB hiccup: don't clobber settings with defaults
            s->chat_font_size = 0;
            s->chat_bg.clear();
            s->chat_user_bg.clear();
            s->chat_user_text.clear();
            s->chat_assistant_bg.clear();
            s->chat_assistant_text.clear();
            save_interface_guarded(services_.store, *s);
            apply_chat();
            *resetting = true;
            chat_font_dd->set_selected(0);
            for (const auto& [btn, row] : color_btns) {
                Gdk::RGBA rgba;
                rgba.set(fallback_for(*row, services_.theme.current()));
                btn->set_rgba(rgba);
            }
            *resetting = false;
            persist();
        });
    box->append(*reset_btn);

    finish_settings_page(*scroll, *box);
    return scroll;
}

// ── Tab 2: Providers (§4.5) ─────────────────────────────────────────
Gtk::Widget* SettingsPanel::build_providers_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(12);

    box->append(*group_header("CONFIGURED PROVIDERS"));
    providers_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->append(*providers_list_);

    // action row: + Add Provider, quick-fill presets
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto open_dialog = [this](std::optional<UiProvider> preset) {
        auto* dlg = Gtk::make_managed<ProviderEditDialog>(
            *toplevel_of(*this), services_, preset);
        dlg->signal_saved().connect([this] { refresh_providers(); });
        dlg->present();
    };
    auto* add = Gtk::make_managed<Gtk::Button>("+ Add Provider");
    add->signal_clicked().connect([open_dialog] { open_dialog(std::nullopt); });
    auto* minimax = Gtk::make_managed<Gtk::Button>("+ MiniMax");
    minimax->signal_clicked().connect(
        [open_dialog] { open_dialog(ProviderEditDialog::preset_minimax()); });
    auto* openai = Gtk::make_managed<Gtk::Button>("+ OpenAI");
    openai->signal_clicked().connect(
        [open_dialog] { open_dialog(ProviderEditDialog::preset_openai()); });
    auto* ollama = Gtk::make_managed<Gtk::Button>("+ Ollama");
    ollama->signal_clicked().connect(
        [open_dialog] { open_dialog(ProviderEditDialog::preset_ollama()); });
    auto* kimi = Gtk::make_managed<Gtk::Button>("+ Kimi");
    kimi->signal_clicked().connect(
        [open_dialog] { open_dialog(ProviderEditDialog::preset_kimi()); });
    auto* lmstudio = Gtk::make_managed<Gtk::Button>("+ LM Studio");
    lmstudio->signal_clicked().connect(
        [open_dialog] { open_dialog(ProviderEditDialog::preset_lmstudio()); });
    actions->append(*add);
    actions->append(*minimax);
    actions->append(*openai);
    actions->append(*ollama);
    actions->append(*kimi);
    actions->append(*lmstudio);
    box->append(*actions);

    finish_settings_page(*scroll, *box);
    refresh_providers();
    return scroll;
}

void SettingsPanel::refresh_providers() {
    std::vector<UiProvider> providers;
    ui_guard("refresh_providers", [&] {
        providers =
            model_bridge::list_providers(daedalus::SettingsStore::instance());
    });

    while (auto* child = providers_list_->get_first_child())
        providers_list_->remove(*child);

    for (const auto& p : providers) {
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        card->add_css_class("card");
        card->set_margin_bottom(4);

        // header row: icon + name + Fetch/Edit/Delete
        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>(
            provider_icon(p.name) + " " + p.name);
        name->set_halign(Gtk::Align::START);
        name->set_hexpand(true);
        head->append(*name);
        // §4.5 — discover models from the provider's /v1/models endpoint.
        auto* fetch = Gtk::make_managed<Gtk::Button>("Fetch Models");
        fetch->set_has_frame(false);
        fetch->signal_clicked().connect([this, p] {
            fetch_models_async(toplevel_of(*this), p,
                               [this] { refresh_models(); });
        });
        head->append(*fetch);
        auto* edit = Gtk::make_managed<Gtk::Button>("Edit");
        edit->set_has_frame(false);
        edit->signal_clicked().connect([this, p] {
            auto* dlg = Gtk::make_managed<ProviderEditDialog>(
                *toplevel_of(*this), services_, p);
            dlg->signal_saved().connect([this] { refresh_providers(); });
            dlg->present();
        });
        auto* del = Gtk::make_managed<Gtk::Button>("Delete");
        del->set_has_frame(false);
        del->signal_clicked().connect([this, pname = p.name] {
            confirm_delete(toplevel_of(*this),
                           "Delete provider \"" + pname +
                               "\" and its models?",
                           [this, pname] {
                               model_bridge::remove_provider(
                                   daedalus::SettingsStore::instance(),
                                   services_.store, pname);
                               refresh_providers();
                               refresh_models();
                           });
        });
        head->append(*edit);
        head->append(*del);
        card->append(*head);

        auto detail = [&](const Glib::ustring& key, const std::string& value) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto* k = Gtk::make_managed<Gtk::Label>(key);
            k->add_css_class("dim-label");
            k->set_halign(Gtk::Align::START);
            k->set_size_request(110, -1);
            auto* v = Gtk::make_managed<Gtk::Label>(value);
            v->set_halign(Gtk::Align::START);
            v->set_ellipsize(Pango::EllipsizeMode::END);
            row->append(*k);
            row->append(*v);
            card->append(*row);
        };
        detail("BASE URL", p.host);
        detail("ENDPOINT", p.chat_endpoint);

        // §ProviderPlugins — effective plugin: the explicit choice, or the
        // registry's auto-detection shown as "(auto: name)".
        std::string plugin_display = p.plugin;
        if (plugin_display.empty()) {
            plugin_display = "(auto: " +
                daedalus::ProviderRegistry::resolve(p.name, p.host)->name() + ")";
        }
        detail("PLUGIN", plugin_display);

        const bool env_key = p.api_key.find("${") != std::string::npos;
        std::string key_display;
        if (env_key) key_display = "env: " + p.api_key;
        else if (!p.api_key.empty())
            key_display = "•••••••• (" + std::to_string(p.api_key.size()) + " chars)";
        detail("API KEY", key_display);

        providers_list_->append(*card);
    }
}

// ── Tab 3: Models (§4.5) ────────────────────────────────────────────
Gtk::Widget* SettingsPanel::build_models_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(12);

    box->append(*group_header("ACTIVE MODEL"));
    default_model_dropdown_ = Gtk::make_managed<Gtk::DropDown>();
    // The selected row *is* the default model (§4.5).
    default_model_dropdown_->property_selected().signal_changed().connect([this] {
        if (updating_default_) return;
        const unsigned idx = default_model_dropdown_->get_selected();
        const auto models = model_bridge::list_models(
            daedalus::SettingsStore::instance(), services_.store);
        if (idx == GTK_INVALID_LIST_POSITION || idx >= models.size()) return;
        model_bridge::set_default_model(daedalus::SettingsStore::instance(),
                                        models[idx].name);
        // Defer the rebuild: swapping the dropdown's model inside its own
        // notify::selected emission makes GTK re-emit the notification,
        // looping back into this handler and freezing the UI.
        Glib::signal_idle().connect_once(
            [this] { refresh_models(); }); // update Default badges on the cards
    });
    box->append(*field_row("Default Model", *default_model_dropdown_));

    box->append(*group_header("CONFIGURED MODELS"));
    models_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->append(*models_list_);

    // action row: + Add Model (endpoint/key come from the provider)
    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* add = Gtk::make_managed<Gtk::Button>("+ Add Model");
    add->signal_clicked().connect([this] {
        // The dialog ctor reads the provider registry from SQLite — guard.
        ui_guard("add model dialog", [&] {
            auto* dlg = Gtk::make_managed<ModelEditDialog>(
                *toplevel_of(*this), services_, std::nullopt);
            dlg->signal_saved().connect([this] { refresh_models(); });
            dlg->present();
        });
    });
    actions->append(*add);
    box->append(*actions);

    finish_settings_page(*scroll, *box);
    refresh_models();
    return scroll;
}

void SettingsPanel::refresh_models() {
    ui_guard("refresh_models", [this] {
    const auto models = model_bridge::list_models(
        daedalus::SettingsStore::instance(), services_.store);
    const auto prompts = services_.store.list_system_prompts();

    // default-model dropdown
    auto names = Gtk::StringList::create({});
    unsigned default_idx = 0;
    for (size_t i = 0; i < models.size(); ++i) {
        names->append(model_bridge::display_name(models[i]));
        if (models[i].is_default) default_idx = static_cast<unsigned>(i);
    }
    updating_default_ = true;
    default_model_dropdown_->set_model(names);
    if (!models.empty()) default_model_dropdown_->set_selected(default_idx);
    updating_default_ = false;

    // cards
    while (auto* child = models_list_->get_first_child())
        models_list_->remove(*child);

    for (const auto& m : models) {
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        card->add_css_class("card");
        card->set_margin_bottom(4);

        // header row: icon + provider/model + Edit/Delete (+Default badge)
        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>(
            provider_icon(m.provider) + " " + m.name + "\n" + m.model_id);
        name->set_halign(Gtk::Align::START);
        name->set_hexpand(true);
        head->append(*name);
        if (m.is_default) {
            auto* badge = Gtk::make_managed<Gtk::Label>("Default");
            badge->add_css_class("accent");
            head->append(*badge);
        }
        auto* edit = Gtk::make_managed<Gtk::Button>("Edit");
        edit->set_has_frame(false);
        edit->signal_clicked().connect([this, m] {
            // The dialog ctor reads the provider registry from SQLite — guard.
            ui_guard("edit model dialog", [&] {
                auto* dlg = Gtk::make_managed<ModelEditDialog>(
                    *toplevel_of(*this), services_, m);
                dlg->signal_saved().connect([this] { refresh_models(); });
                dlg->present();
            });
        });
        auto* del = Gtk::make_managed<Gtk::Button>("Delete");
        del->set_has_frame(false);
        del->signal_clicked().connect([this, name = m.name] {
            confirm_delete(toplevel_of(*this), "Delete model \"" + name + "\"?",
                           [this, name] {
                               model_bridge::remove_model(
                                   daedalus::SettingsStore::instance(),
                                   services_.store, name);
                               refresh_models();
                           });
        });
        head->append(*edit);
        head->append(*del);
        card->append(*head);

        auto detail = [&](const Glib::ustring& key, const std::string& value) {
            auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            auto* k = Gtk::make_managed<Gtk::Label>(key);
            k->add_css_class("dim-label");
            k->set_halign(Gtk::Align::START);
            k->set_size_request(110, -1);
            auto* v = Gtk::make_managed<Gtk::Label>(value);
            v->set_halign(Gtk::Align::START);
            v->set_ellipsize(Pango::EllipsizeMode::END);
            row->append(*k);
            row->append(*v);
            card->append(*row);
        };
        detail("PROVIDER", m.provider);
        detail("CONTEXT / TEMP",
               std::to_string(m.max_context_size) + " / " +
                   std::to_string(m.temperature));

        // per-card system-prompt dropdown (§4.5)
        auto prompt_names = Gtk::StringList::create({"Default"});
        unsigned sel = 0;
        for (size_t i = 0; i < prompts.size(); ++i) {
            prompt_names->append(prompts[i].name);
            if (prompts[i].id == m.system_prompt_id)
                sel = static_cast<unsigned>(i + 1);
        }
        auto* prompt_dd = Gtk::make_managed<Gtk::DropDown>(prompt_names);
        prompt_dd->set_selected(sel);
        const std::string model_name = m.name;
        prompt_dd->property_selected().signal_changed().connect(
            [this, model_name, prompts, prompt_dd] {
                const unsigned idx = prompt_dd->get_selected();
                const long id = (idx == 0 || idx > prompts.size())
                    ? 0
                    : prompts[idx - 1].id;
                ui_guard("set_model_system_prompt", [&] {
                    model_bridge::set_model_system_prompt(services_.store,
                                                          model_name, id);
                });
            });
        card->append(*field_row("SYSTEM PROMPT", *prompt_dd));

        models_list_->append(*card);
    }
    });
}

// ── Tab 4: System Prompts (§4.5) ────────────────────────────────────
// ── Workspace tab (Plan.md P6) ─────────────────────────────────────────
// Two prompt layers, one page:
//   1. Workspace types — the file-based library that focuses the agent per
//      workspace (system prompt + templates + detection). Edit Prompt
//      writes back to the type's .md file; files stay the source of truth.
//   2. Model prompt overrides (advanced) — the §4.5 per-model bindings;
//      these always win over the workspace type prompt.
Gtk::Widget* SettingsPanel::build_workspace_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);

    auto* types_heading = Gtk::make_managed<Gtk::Label>("Workspace types");
    types_heading->set_halign(Gtk::Align::START);
    box->append(*types_heading);
    auto* blurb = Gtk::make_managed<Gtk::Label>(
        "A project type focuses the agent per workspace (system prompt, "
        "file templates). Types are plain Markdown files with front matter "
        "in the types folder — create, edit and share them like any file. "
        "Set a workspace's type via the Workspace panel '+' menu or when "
        "opening a folder; it is stamped into AGENTS.md.");
    blurb->set_wrap(true);
    blurb->set_halign(Gtk::Align::START);
    blurb->add_css_class("dim-label");
    box->append(*blurb);

    types_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->append(*types_list_);

    auto* actions = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* rescan = Gtk::make_managed<Gtk::Button>("Rescan");
    rescan->signal_clicked().connect([this] { refresh_project_types(); });
    auto* open_dir = Gtk::make_managed<Gtk::Button>("Open Types Folder");
    open_dir->signal_clicked().connect([] {
        const auto dir = ProjectTypeLibrary::user_types_dir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (char* uri = g_filename_to_uri(dir.c_str(), nullptr, nullptr)) {
            Gio::AppInfo::launch_default_for_uri(uri);
            g_free(uri);
        }
    });
    auto* new_type = Gtk::make_managed<Gtk::Button>("+ New Type");
    new_type->add_css_class("suggested-action");
    new_type->signal_clicked().connect([this] {
        // Minimal scaffold dialog: id + display name, writes <id>.md.
        auto* dlg = Gtk::make_managed<Gtk::Window>();
        dlg->set_title("New Project Type");
        dlg->set_transient_for(*toplevel_of(*this));
        dlg->set_modal(true);
        auto* v = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
        v->set_margin(16);
        auto* id_entry = Gtk::make_managed<Gtk::Entry>();
        id_entry->set_placeholder_text("id (file name, e.g. podcast)");
        auto* name_entry = Gtk::make_managed<Gtk::Entry>();
        name_entry->set_placeholder_text("Display name (e.g. Podcast)");
        auto* status = Gtk::make_managed<Gtk::Label>();
        status->add_css_class("error-label");
        status->set_halign(Gtk::Align::START);
        auto* create = Gtk::make_managed<Gtk::Button>("Create");
        create->add_css_class("suggested-action");
        create->set_halign(Gtk::Align::END);
        create->signal_clicked().connect([this, dlg, id_entry, name_entry, status] {
            std::string id = id_entry->get_text();
            std::string name = name_entry->get_text();
            const auto ok_id = !id.empty() && id.find_first_not_of(
                "abcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos;
            if (!ok_id) { status->set_text("id: lowercase letters, digits, - or _ only."); return; }
            if (name.empty()) name = id;
            const auto path = ProjectTypeLibrary::user_types_dir() + "/" + id + ".md";
            if (std::filesystem::exists(path)) { status->set_text("That id already exists."); return; }
            std::ofstream f(path, std::ios::trunc);
            if (!f) { status->set_text("Cannot write " + path); return; }
            f << "---\nid: " << id << "\nname: " << name
              << "\ndescription: \ndetect: \n---\n\n"
              << "System prompt for the agent when this project type is "
                 "active.\n";
            f.close();
            dlg->hide();
            refresh_project_types();
        });
        v->append(*id_entry);
        v->append(*name_entry);
        v->append(*status);
        v->append(*create);
        dlg->set_child(*v);
        dlg->present();
    });
    actions->append(*rescan);
    actions->append(*open_dir);
    actions->append(*new_type);
    box->append(*actions);

    box->append(*Gtk::make_managed<Gtk::Separator>());

    auto* ov_heading = Gtk::make_managed<Gtk::Label>("Model prompt overrides (advanced)");
    ov_heading->set_halign(Gtk::Align::START);
    box->append(*ov_heading);
    auto* ov_blurb = Gtk::make_managed<Gtk::Label>(
        "Prompts bound to a model in the Models tab. A bound prompt "
        "replaces the workspace type prompt entirely for that model.");
    ov_blurb->set_wrap(true);
    ov_blurb->set_halign(Gtk::Align::START);
    ov_blurb->add_css_class("dim-label");
    box->append(*ov_blurb);

    prompts_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->append(*prompts_list_);

    auto* add = Gtk::make_managed<Gtk::Button>("+ New Override Prompt");
    add->signal_clicked().connect([this] {
        auto* dlg = Gtk::make_managed<SystemPromptDialog>(
            *toplevel_of(*this), services_, std::nullopt);
        dlg->signal_saved().connect([this] { refresh_prompts(); });
        dlg->present();
    });
    box->append(*add);

    finish_settings_page(*scroll, *box);
    refresh_project_types();
    refresh_prompts();
    return scroll;
}

void SettingsPanel::refresh_prompts() {
    while (auto* child = prompts_list_->get_first_child())
        prompts_list_->remove(*child);

    std::vector<SystemPrompt> prompts;
    ui_guard("refresh_prompts",
             [&] { prompts = services_.store.list_system_prompts(); });
    for (const auto& p : prompts) {
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");

        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>(p.name);
        name->set_halign(Gtk::Align::START);
        name->set_hexpand(true);
        head->append(*name);
        auto* edit = Gtk::make_managed<Gtk::Button>("Edit");
        edit->set_has_frame(false);
        edit->signal_clicked().connect([this, p] {
            auto* dlg = Gtk::make_managed<SystemPromptDialog>(
                *toplevel_of(*this), services_, p);
            dlg->signal_saved().connect([this] { refresh_prompts(); });
            dlg->present();
        });
        auto* del = Gtk::make_managed<Gtk::Button>("Delete");
        del->set_has_frame(false);
        del->signal_clicked().connect([this, id = p.id, name = p.name] {
            confirm_delete(toplevel_of(*this), "Delete prompt \"" + name + "\"?",
                           [this, id] {
                               services_.store.delete_system_prompt(id);
                               refresh_prompts();
                           });
        });
        head->append(*edit);
        head->append(*del);
        card->append(*head);

        if (p.is_default) {
            auto* badge = Gtk::make_managed<Gtk::Label>("[ Default ]");
            badge->set_halign(Gtk::Align::START);
            badge->add_css_class("accent");
            card->append(*badge);
        }

        std::string preview = p.prompt.substr(0, 300);
        if (p.prompt.size() > 300) preview += "…";
        auto* body = Gtk::make_managed<Gtk::Label>(preview);
        body->set_halign(Gtk::Align::START);
        body->set_wrap(true);
        body->add_css_class("dim-label");
        card->append(*body);

        prompts_list_->append(*card);
    }
}

// ── Tab 5: MCP Servers (§4.5) ───────────────────────────────────────
Gtk::Widget* SettingsPanel::build_mcp_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);

    mcp_list_ = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->append(*mcp_list_);

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* add = Gtk::make_managed<Gtk::Button>("+ Add MCP Server");
    add->signal_clicked().connect([this] {
        auto* dlg = Gtk::make_managed<McpServerDialog>(*toplevel_of(*this), std::nullopt);
        dlg->signal_saved().connect([this] {
            refresh_mcp();
            signal_agent_config_changed_.emit();
        });
        dlg->present();
    });
    buttons->append(*add);

    auto* add_json = Gtk::make_managed<Gtk::Button>("+ Add from JSON…");
    add_json->signal_clicked().connect([this] {
        auto* dlg = Gtk::make_managed<McpJsonImportDialog>(*toplevel_of(*this));
        dlg->signal_saved().connect([this] {
            refresh_mcp();
            signal_agent_config_changed_.emit();
        });
        dlg->present();
    });
    buttons->append(*add_json);
    box->append(*buttons);

    finish_settings_page(*scroll, *box);
    refresh_mcp();
    return scroll;
}

void SettingsPanel::refresh_mcp() {
    while (auto* child = mcp_list_->get_first_child())
        mcp_list_->remove(*child);

    std::vector<McpServer> servers;
    ui_guard("refresh_mcp", [&] {
        servers = mcp_bridge::list(daedalus::SettingsStore::instance());
    });
    for (const auto& s : servers) {
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");

        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>(s.name);
        name->set_halign(Gtk::Align::START);
        name->set_hexpand(true);
        head->append(*name);
        // §4.5 — per-server enable/disable; persisted in the daedalus
        // registry and passed to the agent via AgentConfig on rebuild.
        auto* toggle = Gtk::make_managed<Gtk::Switch>();
        toggle->set_active(s.enabled);
        toggle->set_valign(Gtk::Align::CENTER);
        toggle->property_active().signal_changed().connect([this, s, toggle] {
            if (toggle->get_active() == s.enabled) return; // initial sync
            McpServer updated = s;
            updated.enabled = toggle->get_active();
            ui_guard("mcp enable toggle", [&] {
                mcp_bridge::save(daedalus::SettingsStore::instance(), updated);
            });
            refresh_mcp();
            signal_agent_config_changed_.emit();
        });
        head->append(*toggle);
        auto* edit = Gtk::make_managed<Gtk::Button>("✎");
        edit->set_has_frame(false);
        edit->signal_clicked().connect([this, s] {
            auto* dlg = Gtk::make_managed<McpServerDialog>(*toplevel_of(*this), s);
            dlg->signal_saved().connect([this] {
                refresh_mcp();
                signal_agent_config_changed_.emit();
            });
            dlg->present();
        });
        auto* del = Gtk::make_managed<Gtk::Button>("×");
        del->set_has_frame(false);
        del->signal_clicked().connect([this, name = s.name] {
            confirm_delete(toplevel_of(*this), "Delete MCP server \"" + name + "\"?",
                           [this, name] {
                               mcp_bridge::remove(daedalus::SettingsStore::instance(), name);
                               refresh_mcp();
                               signal_agent_config_changed_.emit();
                           });
        });
        head->append(*edit);
        head->append(*del);
        card->append(*head);

        auto* cmd_label = Gtk::make_managed<Gtk::Label>("COMMAND");
        cmd_label->set_halign(Gtk::Align::START);
        cmd_label->add_css_class("dim-label");
        auto* cmd = Gtk::make_managed<Gtk::Label>(s.command);
        cmd->set_halign(Gtk::Align::START);
        cmd->add_css_class("monospace");
        cmd->set_ellipsize(Pango::EllipsizeMode::END);
        card->append(*cmd_label);
        card->append(*cmd);

        // env var names only (§4.5)
        std::string env_names;
        const auto env = nlohmann::json::parse(s.env_json, nullptr, false);
        if (env.is_object()) {
            for (auto it = env.begin(); it != env.end(); ++it) {
                if (!env_names.empty()) env_names += ", ";
                env_names += it.key();
            }
        }
        if (!env_names.empty()) {
            auto* env_label = Gtk::make_managed<Gtk::Label>("ENVIRONMENT");
            env_label->set_halign(Gtk::Align::START);
            env_label->add_css_class("dim-label");
            auto* env_val = Gtk::make_managed<Gtk::Label>(env_names);
            env_val->set_halign(Gtk::Align::START);
            card->append(*env_label);
            card->append(*env_val);
        }

        mcp_list_->append(*card);
    }
}

// ── Tab 6: Agent (§4.5) ─────────────────────────────────────────────
// Row with a trailing enable/disable switch (skills, tools, MCP master).
Gtk::Widget* SettingsPanel::make_toggle_row(
    const std::string& title, const std::string& subtitle, bool enabled,
    std::function<void(bool)> on_toggle) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* texts = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
    texts->set_hexpand(true);
    auto* t = Gtk::make_managed<Gtk::Label>(title);
    t->set_halign(Gtk::Align::START);
    texts->append(*t);
    if (!subtitle.empty()) {
        auto* s = Gtk::make_managed<Gtk::Label>(subtitle);
        s->set_halign(Gtk::Align::START);
        s->set_wrap(true);
        s->add_css_class("dim-label");
        texts->append(*s);
    }
    row->append(*texts);
    auto* sw = Gtk::make_managed<Gtk::Switch>();
    sw->set_active(enabled);
    sw->set_valign(Gtk::Align::CENTER);
    sw->property_active().signal_changed().connect(
        [sw, on_toggle = std::move(on_toggle)] {
            on_toggle(sw->get_active());
        });
    row->append(*sw);
    return row;
}

void SettingsPanel::toggle_skill(const std::string& name, bool enabled) {
    ui_guard("toggle_skill", [&] {
        auto disabled = services_.store.load_disabled_skills();
        std::erase(disabled, name);
        if (!enabled) disabled.push_back(name);
        services_.store.save_disabled_skills(disabled);
    });
    signal_agent_config_changed_.emit();
}

void SettingsPanel::toggle_tool(const std::string& name, bool enabled) {
    ui_guard("toggle_tool", [&] {
        auto disabled = services_.store.load_disabled_tools();
        std::erase(disabled, name);
        if (!enabled) disabled.push_back(name);
        services_.store.save_disabled_tools(disabled);
    });
    signal_agent_config_changed_.emit();
}

Gtk::Widget* SettingsPanel::build_agent_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(12);

    auto in = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    // ── BEHAVIOR: bash auto-approve gate (ChatController reads
    //    agent.auto_approve on every agent build) ──────────────────────
    box->append(*group_header("BEHAVIOR"));
    const bool auto_approve =
        services_.store.get("agent.auto_approve").value_or("0") == "1";
    box->append(*make_toggle_row(
        "Auto-approve Bash",
        "When on, the agent runs bash commands without asking.",
        auto_approve,
        [this](bool on) {
            services_.store.set("agent.auto_approve", on ? "1" : "0");
        }));

    // ── SKILLS: per-skill enable/disable (host policy, §4.5) ─────────
    box->append(*group_header("SKILLS"));
    const auto disabled_skills = services_.store.load_disabled_skills();
    const auto skills = discover_global_skills();
    if (skills.empty()) {
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "No skills found in the global skills directory.");
        hint->set_halign(Gtk::Align::START);
        hint->set_margin_start(8);
        hint->add_css_class("dim-label");
        box->append(*hint);
    }
    for (const auto& sk : skills) {
        box->append(*make_toggle_row(
            sk.name, sk.description, !in(disabled_skills, sk.name),
            [this, name = sk.name](bool on) { toggle_skill(name, on); }));
    }

    // ── TOOLS: per-tool enable/disable; full list needs a live agent ──
    box->append(*group_header("TOOLS"));
    const auto disabled_tools = services_.store.load_disabled_tools();
    std::vector<std::string> tool_names;
    std::unordered_map<std::string, std::string> tool_descs;
    if (tool_list_provider_) {
        for (const auto& t : tool_list_provider_()) {
            tool_names.push_back(t.name);
            tool_descs[t.name] = t.description;
        }
    }
    // keep disabled entries visible even without a live agent
    std::copy_if(disabled_tools.begin(), disabled_tools.end(),
                 std::back_inserter(tool_names),
                 [&](const std::string& d) { return !in(tool_names, d); });
    if (tool_names.empty()) {
        auto* hint = Gtk::make_managed<Gtk::Label>(
            "Start a chat session to list the agent's tools.");
        hint->set_halign(Gtk::Align::START);
        hint->set_margin_start(8);
        hint->add_css_class("dim-label");
        box->append(*hint);
    }
    for (const auto& tn : tool_names) {
        box->append(*make_toggle_row(
            tn, tool_descs[tn], !in(disabled_tools, tn),
            [this, tn](bool on) { toggle_tool(tn, on); }));
    }

    // ── LIMITS + MCP master switch ────────────────────────────────────
    box->append(*group_header("LIMITS"));
    auto& registry = daedalus::SettingsStore::instance();
    auto limit_row = [this](const Glib::ustring& label, double lo, double hi,
                            double value, std::function<void(int)> apply) {
        auto* spin = Gtk::make_managed<Gtk::SpinButton>();
        spin->set_range(lo, hi);
        spin->set_increments(1, 10);
        spin->set_value(value);
        spin->signal_value_changed().connect(
            [this, spin, apply = std::move(apply)] {
                ui_guard("agent limit", [&] { apply(spin->get_value_as_int()); });
                signal_agent_config_changed_.emit();
            });
        return field_row(label, *spin);
    };
    box->append(*limit_row("Max agent iterations", 1, 1000,
                           registry.get_max_agent_iterations(),
                           [](int v) {
                               daedalus::SettingsStore::instance()
                                   .set_max_agent_iterations(v);
                           }));
    box->append(*limit_row("Max tool result (KB)", 1, 1024,
                           registry.get_max_tool_result_size() / 1024.0,
                           [](int v) {
                               daedalus::SettingsStore::instance()
                                   .set_max_tool_result_size(
                                       static_cast<size_t>(v) * 1024);
                           }));
    box->append(*limit_row("Max request (MB)", 1, 64,
                           registry.get_max_request_size() / 1048576.0,
                           [](int v) {
                               daedalus::SettingsStore::instance()
                                   .set_max_request_size(
                                       static_cast<size_t>(v) * 1048576);
                           }));
    box->append(*limit_row("Max response (MB)", 1, 64,
                           registry.get_max_response_size() / 1048576.0,
                           [](int v) {
                               daedalus::SettingsStore::instance()
                                   .set_max_response_size(
                                       static_cast<size_t>(v) * 1048576);
                           }));

    box->append(*group_header("MCP"));
    const bool mcp_on =
        services_.store.get("agent.mcp_enabled").value_or("1") == "1";
    box->append(*make_toggle_row(
        "MCP servers enabled",
        "Master switch — off disables all MCP tool integrations.",
        mcp_on,
        [this](bool on) {
            services_.store.set("agent.mcp_enabled", on ? "1" : "0");
            signal_agent_config_changed_.emit();
        }));

    finish_settings_page(*scroll, *box);
    return scroll;
}

// ── Tab 7: Search (§4.3) ────────────────────────────────────────────
// ── Tab 7: Web Search (§WEB-SEARCH-CORE, 2026-10-10) ────────────────
// Provider failover chain for the core web_search tool: per-provider
// enable/disable, credentials, and priority order. Reads/writes the
// SHARED daedalus SettingsStore (schema v8 web_search_providers table),
// so icarus and the settings TUI see the same configuration.
Gtk::Widget* SettingsPanel::build_web_search_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);

    box->append(*group_header("WEB SEARCH PROVIDERS"));
    auto* hint = Gtk::make_managed<Gtk::Label>(
        "The web_search tool tries enabled providers in priority order "
        "(lowest number first) until one returns results. The hybrid "
        "provider (Bing/DuckDuckGo HTML) needs no key, SearXNG needs your "
        "instance URL, the rest need an "
        "API key. Agents with a built-in browser can also search the web "
        "like a human — this chain backs the web_search tool itself. "
        "Note: SearXNG itself has NO API keys — its 'API key/token' field "
        "is optional and only for instances behind a token-gating reverse "
        "proxy; a Basic-auth instance uses the user/pass fields instead.");
    hint->set_halign(Gtk::Align::START);
    hint->set_wrap(true);
    hint->add_css_class("dim-label");
    box->append(*hint);

    static const std::map<std::string, std::string> display_names = {
        {"brave", "Brave Search"},     {"tavily", "Tavily"},
        {"exa", "Exa"},                {"firecrawl", "Firecrawl"},
        {"serper", "Serper (Google)"}, {"serpapi", "SerpApi (Google)"},
        {"searxng", "SearXNG (self-hosted)"},
        {"hybrid", "Web HTML (Bing/DuckDuckGo)"},
    };

    // Re-read the row, mutate one field, write it back — keystroke-level
    // auto-save without stale-copy clobbering between fields.
    auto save_field =
        [this](const std::string& name,
               const std::function<void(
                   daedalus::WebSearchProviderSettings&)>& mutate) {
            ui_guard("save_web_search_provider", [&] {
                auto& store = daedalus::SettingsStore::instance();
                for (auto& row : store.get_web_search_providers()) {
                    if (row.name == name) {
                        mutate(row);
                        store.set_web_search_provider(row);
                        break;
                    }
                }
            });
        };

    auto entry_row =
        [](const std::string& label, const std::string& value, bool secret,
           std::function<void(const std::string&)> on_change) {
            auto* row =
                Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            row->set_margin_start(8);
            auto* l = Gtk::make_managed<Gtk::Label>(label);
            l->set_halign(Gtk::Align::START);
            l->set_size_request(130, -1);
            row->append(*l);
            auto* e = Gtk::make_managed<Gtk::Entry>();
            e->set_hexpand(true);
            e->set_text(value);
            if (secret) {
                e->set_visibility(false);
                e->set_input_purpose(Gtk::InputPurpose::PASSWORD);
            }
            e->signal_changed().connect([e, on_change = std::move(on_change)] {
                on_change(e->get_text());
            });
            row->append(*e);
            return row;
        };

    std::vector<daedalus::WebSearchProviderSettings> rows;
    ui_guard("load_web_search_providers", [&] {
        rows =
            daedalus::SettingsStore::instance().get_web_search_providers();
    });

    for (const auto& p : rows) {
        auto* card =
            Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 6);
        card->add_css_class("card");
        card->set_margin_bottom(4);

        const auto dn = display_names.find(p.name);
        const std::string dname =
            dn != display_names.end() ? dn->second : p.name;
        std::string sub = "priority " + std::to_string(p.priority);
        if (p.name == "hybrid") {
            sub += " · free, no key needed";
        } else if (p.name == "searxng") {
            sub += p.base_url.empty() ? " · needs instance URL"
                                      : " · " + p.base_url;
            if (!p.base_url.empty())
                sub += p.username.empty() && p.api_key.empty()
                           ? " · no auth"
                           : (p.username.empty() ? " · token auth"
                                                 : " · basic auth");
        } else {
            sub += p.api_key.empty() ? " · needs API key" : " · key set";
        }

        card->append(*make_toggle_row(
            dname, sub, p.enabled, [save_field, name = p.name](bool on) {
                save_field(name,
                           [on](auto& r) { r.enabled = on; });
            }));

        if (p.name == "searxng") {
            card->append(*entry_row(
                "Instance URL", p.base_url, false,
                [save_field, name = p.name](const std::string& v) {
                    save_field(name, [v](auto& r) { r.base_url = v; });
                }));
            card->append(*entry_row(
                "Token (optional)", p.api_key, true,
                [save_field, name = p.name](const std::string& v) {
                    save_field(name, [v](auto& r) { r.api_key = v; });
                }));
            card->append(*entry_row(
                "Basic user", p.username, false,
                [save_field, name = p.name](const std::string& v) {
                    save_field(name, [v](auto& r) { r.username = v; });
                }));
            card->append(*entry_row(
                "Basic pass", p.password, true,
                [save_field, name = p.name](const std::string& v) {
                    save_field(name, [v](auto& r) { r.password = v; });
                }));
        } else if (p.name != "hybrid") {
            card->append(*entry_row(
                "API key", p.api_key, true,
                [save_field, name = p.name](const std::string& v) {
                    save_field(name, [v](auto& r) { r.api_key = v; });
                }));
        }
        card->append(*entry_row(
            "Priority", std::to_string(p.priority), false,
            [save_field, name = p.name](const std::string& v) {
                int n = 0;
                try {
                    n = std::stoi(v);
                } catch (...) {
                    return;  // partial input mid-edit — ignore
                }
                save_field(name, [n](auto& r) { r.priority = n; });
            }));

        box->append(*card);
    }

    finish_settings_page(*scroll, *box);
    return scroll;
}

Gtk::Widget* SettingsPanel::build_search_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    box->set_margin(12);

    box->append(*group_header("SEARCH"));

    auto* hint = Gtk::make_managed<Gtk::Label>(
        "Exclude patterns — one per line. Matched against file names and "
        "workspace-relative paths. Wildcards: * ?");
    hint->set_halign(Gtk::Align::START);
    hint->set_wrap(true);
    hint->add_css_class("dim-label");
    box->append(*hint);

    auto* view = Gtk::make_managed<Gtk::TextView>();
    view->add_css_class("monospace");
    view->set_top_margin(6);
    view->set_bottom_margin(6);
    view->set_left_margin(6);
    view->set_right_margin(6);
    auto* text_scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    text_scroll->set_size_request(-1, 260);
    text_scroll->set_child(*view);
    box->append(*text_scroll);

    auto join = [](const std::vector<std::string>& v) {
        std::string out;
        for (const auto& s : v) {
            if (!out.empty()) out += '\n';
            out += s;
        }
        return out;
    };
    view->get_buffer()->set_text(join(services_.store.load_search_excludes()));
    // auto-save on edit, like the other settings (§4.5)
    view->get_buffer()->signal_changed().connect([this, view] {
        std::vector<std::string> patterns;
        std::string line;
        std::istringstream in(view->get_buffer()->get_text());
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) patterns.push_back(line);
        }
        ui_guard("save_search_excludes",
                 [&] { services_.store.save_search_excludes(patterns); });
    });

    auto* reset = Gtk::make_managed<Gtk::Button>("Reset to Defaults");
    reset->set_halign(Gtk::Align::START);
    reset->signal_clicked().connect([view, join] {
        view->get_buffer()->set_text(
            join(SettingsStore::default_search_excludes())); // persists via changed
    });
    box->append(*reset);

    finish_settings_page(*scroll, *box);
    return scroll;
}

// ── Tab 8: Paths (§4.5) ─────────────────────────────────────────────
// Read-only listing of every filesystem location MonkeyByte and its
// embedded libraries use. Not affected by the Save button.
Gtk::Widget* SettingsPanel::build_paths_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(12);

    // One entry: name + description, then the path itself (selectable so
    // the user can copy it). Missing paths are dimmed and marked.
    auto add_path = [box](const Glib::ustring& name, const Glib::ustring& desc,
                          const std::string& path) {
        auto* entry = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        entry->set_margin_start(8);

        auto* title = Gtk::make_managed<Gtk::Label>(name);
        title->set_halign(Gtk::Align::START);
        entry->append(*title);

        auto* d = Gtk::make_managed<Gtk::Label>(desc);
        d->set_halign(Gtk::Align::START);
        d->set_wrap(true);
        d->add_css_class("dim-label");
        entry->append(*d);

        std::error_code ec;
        const bool exists = std::filesystem::exists(path, ec);
        auto* p = Gtk::make_managed<Gtk::Label>(
            exists ? path : path + "  (not created yet)");
        p->set_halign(Gtk::Align::START);
        p->set_hexpand(true);
        p->set_selectable(true);
        p->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        p->add_css_class("monospace");
        if (!exists) p->add_css_class("dim-label");
        entry->append(*p);

        box->append(*entry);
    };

    // ── MonkeyByte ──
    box->append(*group_header("MONKEYBYTE"));
    add_path("Settings database",
             "Authoritative settings: models, MCP servers, system prompts, "
             "workspace (design §10.2).",
             SettingsStore::default_db_path());
    const auto& ws = services_.files.workspace();
    add_path("Current workspace",
             "The project folder open in the editor.",
             ws.empty() ? "(no workspace selected)" : ws.string());
    if (!ws.empty()) {
        ui_guard("paths tab", [&] {
            add_path("Workspace agent data",
                     "daedalus project-tier state for this workspace: agent "
                     "memory and session transcripts.",
                     daedalus::resolve_agent_dir(ws, "").string());
        });
    }
    add_path("GSettings UI cache",
             "Cache-only UI state (window/sidebar/chat widths, theme, "
             "selected model) under schema io.github.monkeybyte.*; stored "
             "by dconf, authoritative state lives in the SQLite DBs above.",
             Glib::get_user_config_dir() + "/dconf/user");

    // ── daedalus (agent backend) ──
    box->append(*group_header("DAEDALUS"));
    const auto& dp = daedalus::Paths::global();
    add_path("Config directory",
             "daedalus config.json and MCP servers.json / .env.",
             dp.user_config_root().string());
    add_path("Data directory",
             "daedalus.db settings store and user-installed skill packs "
             "(design §10.2).",
             dp.user_data_root().string());
    add_path("State directory",
             "Logs, chat history, and usage stats.",
             dp.user_state_root().string());
    add_path("Cache directory",
             "Regenerable caches (response replay, embeddings) — safe to "
             "delete.",
             dp.user_cache_root().string());

    // ── OmniNet (HTTP library bundled via daedalus) ──
    box->append(*group_header("OMNINET"));
    add_path("Config file",
             "OmniNet tooling configuration. Honors OMNINET_DATA_DIR and "
             "the legacy ~/.omninet layout when set.",
             dp.omninet_config_file().string());
    add_path("Data directory",
             "Runtime data used by OmniNet tools.",
             dp.omninet_data_dir().string());

    finish_settings_page(*scroll, *box);
    return scroll;
}

// ── ChimpFlow (§CHIMPFLOW) ──────────────────────────────────────────
Gtk::Widget* SettingsPanel::build_chimpflow_tab() {
    auto* scroll = Gtk::make_managed<SettingsScroll>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(12);

    ChimpFlowSettings current;
    ui_guard("settings load_chimpflow",
             [&] { current = services_.store.load_chimpflow(); });

    box->append(*group_header("CHIMPFLOW"));
    chimpflow_enabled_ = Gtk::make_managed<Gtk::Switch>();
    chimpflow_enabled_->set_active(current.enabled);
    box->append(*field_row("Enable local voice support", *chimpflow_enabled_));
    chimpflow_read_assistant_ = Gtk::make_managed<Gtk::Switch>();
    chimpflow_read_assistant_->set_active(current.read_assistant);
    box->append(*field_row("Read assistant replies aloud", *chimpflow_read_assistant_));
    chimpflow_dictation_ = Gtk::make_managed<Gtk::Switch>();
    chimpflow_dictation_->set_active(current.dictation_enabled);
    box->append(*field_row("Enable microphone dictation", *chimpflow_dictation_));

    box->append(*group_header("CONNECTION"));
    chimpflow_server_ = Gtk::make_managed<Gtk::Entry>();
    chimpflow_server_->set_text(current.server_url);
    chimpflow_server_->set_width_chars(24);
    chimpflow_server_->set_tooltip_text(
        "Loopback only: http://127.0.0.1, http://localhost, or http://[::1]");
    box->append(*field_row("Server URL (no port)", *chimpflow_server_));

    chimpflow_port_ = Gtk::make_managed<Gtk::SpinButton>();
    chimpflow_port_->set_adjustment(
        Gtk::Adjustment::create(current.port, 1, 65535, 1, 100, 0));
    chimpflow_port_->set_numeric(true);
    box->append(*field_row("Port", *chimpflow_port_));

    chimpflow_key_ = Gtk::make_managed<Gtk::PasswordEntry>();
    chimpflow_key_->set_text(current.api_key);
    chimpflow_key_->set_show_peek_icon(true);
    chimpflow_key_->property_placeholder_text().set_value("Scoped key, or empty for CHIMPFLOW_KEY");
    box->append(*field_row("API key (tts + stt scopes)", *chimpflow_key_));

    box->append(*group_header("MODELS & VOICE"));
    chimpflow_tts_model_ = Gtk::make_managed<Gtk::DropDown>();
    replace_dropdown_items(*chimpflow_tts_model_, chimpflow_tts_items_, {},
                           current.tts_model, updating_chimpflow_);
    chimpflow_tts_model_->property_selected().signal_changed().connect([this] {
        if (!updating_chimpflow_) refresh_chimpflow_catalog();
    });
    box->append(*field_row("TTS model", *chimpflow_tts_model_));

    chimpflow_stt_model_ = Gtk::make_managed<Gtk::Entry>();
    chimpflow_stt_model_->set_text(current.stt_model);
    box->append(*field_row("STT model", *chimpflow_stt_model_));

    chimpflow_voice_ = Gtk::make_managed<Gtk::DropDown>();
    replace_dropdown_items(*chimpflow_voice_, chimpflow_voice_items_, {},
                           current.voice, updating_chimpflow_);
    box->append(*field_row("Agent voice", *chimpflow_voice_));
    chimpflow_voice_->property_selected().signal_changed().connect([this] {
        if (!updating_chimpflow_) preview_chimpflow_voice();
    });

    auto* buttons = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    buttons->set_margin_start(8);
    chimpflow_refresh_ = Gtk::make_managed<Gtk::Button>("Refresh Models / Voices");
    chimpflow_refresh_->signal_clicked().connect(
        [this] { refresh_chimpflow_catalog(); });
    chimpflow_preview_ = Gtk::make_managed<Gtk::Button>("Preview Voice");
    chimpflow_preview_->set_sensitive(!current.voice.empty());
    chimpflow_preview_->signal_clicked().connect(
        [this] { preview_chimpflow_voice(); });
    buttons->append(*chimpflow_refresh_);
    buttons->append(*chimpflow_preview_);
    box->append(*buttons);

    chimpflow_status_ = Gtk::make_managed<Gtk::Label>(
        "Discovery and previews run locally. Selecting a voice plays a sample.");
    chimpflow_status_->set_halign(Gtk::Align::START);
    chimpflow_status_->set_wrap(true);
    chimpflow_status_->add_css_class("dim-label");
    box->append(*chimpflow_status_);

    finish_settings_page(*scroll, *box);
    return scroll;
}

ChimpFlowSettings SettingsPanel::chimpflow_from_widgets() const {
    ChimpFlowSettings config;
    config.enabled = chimpflow_enabled_->get_active();
    config.read_assistant = chimpflow_read_assistant_->get_active();
    config.dictation_enabled = chimpflow_dictation_->get_active();
    config.server_url = chimpflow_server_->get_text();
    config.port = chimpflow_port_->get_value_as_int();
    config.api_key = chimpflow_key_->get_text();
    config.tts_model = selected_dropdown_text(*chimpflow_tts_model_);
    config.stt_model = chimpflow_stt_model_->get_text();
    config.voice = selected_dropdown_text(*chimpflow_voice_);
    return config;
}

void SettingsPanel::refresh_chimpflow_catalog() {
    if (chimpflow_discovery_busy_) return;
    const auto config = chimpflow_from_widgets();
    try {
        (void)chimpflow_base_url(config);
    } catch (const std::exception& e) {
        chimpflow_status_->set_text(e.what());
        return;
    }
    chimpflow_discovery_busy_ = true;
    const auto generation = ++chimpflow_discovery_generation_;
    chimpflow_refresh_->set_sensitive(false);
    chimpflow_status_->set_text("Refreshing ChimpFlow models and voices…");
    services_.voice->discover(config,
        [this, generation, alive = alive_](
            const VoiceCatalog& catalog, const std::string& error) {
            if (!*alive || generation != chimpflow_discovery_generation_) return;
            chimpflow_discovery_busy_ = false;
            chimpflow_refresh_->set_sensitive(true);
            if (!error.empty()) {
                chimpflow_status_->set_text(error);
                return;
            }
            const auto current = chimpflow_from_widgets();
            replace_dropdown_items(*chimpflow_tts_model_, chimpflow_tts_items_,
                                   catalog.models, catalog.selected_model,
                                   updating_chimpflow_);
            const bool voice_is_available =
                std::find(catalog.voices.begin(), catalog.voices.end(),
                          current.voice) != catalog.voices.end();
            replace_dropdown_items(*chimpflow_voice_, chimpflow_voice_items_,
                                   catalog.voices,
                                   voice_is_available ? current.voice : "",
                                   updating_chimpflow_);
            chimpflow_preview_->set_sensitive(
                !selected_dropdown_text(*chimpflow_voice_).empty());
            chimpflow_status_->set_text(
                "Discovered " + std::to_string(catalog.models.size()) +
                " model(s) and " + std::to_string(catalog.voices.size()) +
                " voice(s).");
        });
}

void SettingsPanel::preview_chimpflow_voice() {
    auto config = chimpflow_from_widgets();
    if (config.voice.empty()) {
        chimpflow_status_->set_text("Select a ChimpFlow voice first");
        return;
    }
    try {
        (void)chimpflow_base_url(config);
    } catch (const std::exception& e) {
        chimpflow_status_->set_text(e.what());
        return;
    }
    chimpflow_preview_busy_ = true;
    const auto generation = ++chimpflow_preview_generation_;
    chimpflow_preview_->set_sensitive(false);
    chimpflow_status_->set_text("Generating voice preview…");
    services_.voice->preview(config, config.voice,
        [this, generation, alive = alive_](const std::string& error) {
            if (!*alive || generation != chimpflow_preview_generation_) return;
            chimpflow_preview_busy_ = false;
            chimpflow_preview_->set_sensitive(true);
            chimpflow_status_->set_text(error.empty() ? "Voice preview finished" : error);
        });
}

void SettingsPanel::on_save() {
    try {
        const auto config = chimpflow_from_widgets();
        (void)chimpflow_base_url(config);
        if (config.enabled) {
            if (resolved_chimpflow_api_key(config).empty())
                throw std::runtime_error(
                    "ChimpFlow API key is required (or set CHIMPFLOW_KEY)");
            if (config.tts_model.empty())
                throw std::runtime_error("Select a ChimpFlow TTS model");
            if (config.voice.empty())
                throw std::runtime_error("Select a ChimpFlow voice");
            if (config.dictation_enabled && config.stt_model.empty())
                throw std::runtime_error("Enter a ChimpFlow STT model");
        }
        services_.store.save_chimpflow(config);
        ++chimpflow_discovery_generation_;
        ++chimpflow_preview_generation_;
        chimpflow_discovery_busy_ = false;
        chimpflow_preview_busy_ = false;
        chimpflow_refresh_->set_sensitive(true);
        chimpflow_preview_->set_sensitive(!config.voice.empty());
        services_.voice->reload();
        chimpflow_status_->set_text("ChimpFlow settings saved and reloaded");
        status_label_.set_text("Settings saved successfully");
    } catch (const std::exception& e) {
        status_label_.set_text(Glib::ustring("Failed to save: ") + e.what());
    }
}



void SettingsPanel::refresh_project_types() {
    if (!types_list_) return;
    while (auto* child = types_list_->get_first_child())
        types_list_->remove(*child);

    services_.project_types.reload(); // pick up edits/new files
    for (const auto& t : services_.project_types.list()) {
        auto* card = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
        card->add_css_class("card");

        auto* head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        auto* name = Gtk::make_managed<Gtk::Label>(t.name);
        name->set_halign(Gtk::Align::START);
        name->set_hexpand(true);
        auto* badge = Gtk::make_managed<Gtk::Label>(t.id);
        badge->add_css_class("dim-label");
        head->append(*name);
        head->append(*badge);
        card->append(*head);

        if (!t.description.empty()) {
            auto* d = Gtk::make_managed<Gtk::Label>(t.description);
            d->set_halign(Gtk::Align::START);
            d->set_wrap(true);
            d->add_css_class("dim-label");
            card->append(*d);
        }
        std::string meta = t.source_file;
        std::error_code ec;
        if (std::filesystem::is_directory(t.templates_dir, ec))
            meta += "  •  has templates";
        if (!t.detect_patterns.empty()) {
            meta += "  •  detect: ";
            for (size_t i = 0; i < t.detect_patterns.size(); ++i)
                meta += (i ? ", " : "") + t.detect_patterns[i];
        }
        auto* path = Gtk::make_managed<Gtk::Label>(meta);
        path->set_halign(Gtk::Align::START);
        path->add_css_class("dim-label");
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        card->append(*path);

        auto* btn_row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        btn_row->set_halign(Gtk::Align::START);
        // Edit Prompt… — file-backed: rewrites the prompt body of the
        // type's .md, preserving its front matter (update_prompt, P6).
        auto* edit = Gtk::make_managed<Gtk::Button>("Edit Prompt…");
        edit->set_has_frame(false);
        edit->signal_clicked().connect([this, t] {
            auto* dlg = Gtk::make_managed<Gtk::Window>();
            dlg->set_title("Edit Prompt — " + t.name);
            dlg->set_transient_for(*toplevel_of(*this));
            dlg->set_modal(true);
            dlg->set_default_size(520, 420);
            auto* v = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
            v->set_margin(16);
            auto* view = Gtk::make_managed<Gtk::TextView>();
            view->set_wrap_mode(Gtk::WrapMode::WORD_CHAR);
            view->add_css_class("monospace");
            view->get_buffer()->set_text(t.system_prompt);
            auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
            scroller->set_child(*view);
            scroller->set_vexpand(true);
            v->append(*scroller);
            auto* status = Gtk::make_managed<Gtk::Label>();
            status->add_css_class("error-label");
            status->set_halign(Gtk::Align::START);
            v->append(*status);
            auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
            footer->set_halign(Gtk::Align::END);
            auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
            cancel->signal_clicked().connect([dlg] { dlg->hide(); });
            auto* save = Gtk::make_managed<Gtk::Button>("Save");
            save->add_css_class("suggested-action");
            save->signal_clicked().connect([this, dlg, view, t, status] {
                std::string err;
                if (!ProjectTypeLibrary::update_prompt(
                        t, view->get_buffer()->get_text(), &err)) {
                    status->set_text(err);
                    return;
                }
                dlg->hide();
                refresh_project_types(); // rescan picks up the new body
            });
            footer->append(*cancel);
            footer->append(*save);
            v->append(*footer);
            dlg->set_child(*v);
            dlg->present();
        });
        btn_row->append(*edit);
        auto* open = Gtk::make_managed<Gtk::Button>("Open in Editor");
        open->set_has_frame(false);
        open->signal_clicked().connect([this, file = t.source_file] {
            signal_open_file_.emit(file);
        });
        btn_row->append(*open);
        card->append(*btn_row);
        types_list_->append(*card);
    }
}

} // namespace monkeybyte
