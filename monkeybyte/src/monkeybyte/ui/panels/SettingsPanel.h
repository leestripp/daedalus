#pragma once

#include "monkeybyte/backend/SettingsStore.h"

#include <daedalus/types/ToolInfo.h>
#include <cstdint>
#include <functional>
#include <gtkmm.h>

namespace monkeybyte {

struct AppServices;
struct VoiceCatalog;

// §4.5 — Settings: Interface / Providers / Models / Workspace / MCP /
// Agent / Web Search / ChimpFlow / Search / Paths, GtkStack content,
// footer with status + Save.
class SettingsPanel : public Gtk::Box {
public:
    explicit SettingsPanel(AppServices& services);
    ~SettingsPanel() override;

    sigc::signal<void()> signal_theme_changed() { return signal_theme_changed_; }
    sigc::signal<void(const std::string&)> signal_workspace_changed() {
        return signal_workspace_changed_;
    }
    // Explorer-visible settings changed (e.g. show hidden files).
    sigc::signal<void()> signal_tree_refresh() { return signal_tree_refresh_; }
    // Agent-affecting config changed (MCP servers, skills, tools, limits);
    // the running agent must be rebuilt to pick up the change.
    sigc::signal<void()> signal_agent_config_changed() {
        return signal_agent_config_changed_;
    }

    // Source for the Agent tab's tool list — the live agent's tools, or
    // empty when no session exists. Wired by MainWindow to ChatController.
    // Settings → Types "Open in Editor" (Plan.md P5) — forwarded through
    // Sidebar to MainWindow's editor.
    sigc::signal<void(const std::string&)> signal_open_file() {
        return signal_open_file_;
    }

    void set_tool_list_provider(
        std::function<std::vector<daedalus::ToolInfo>()> provider) {
        tool_list_provider_ = std::move(provider);
    }

private:
    enum Tab { Interface = 0, Providers = 1, Models = 2, Workspace = 3, Mcp = 4,
               Agent = 5, WebSearch = 6, ChimpFlow = 7, Search = 8, Paths = 9 };

    Gtk::Widget* build_interface_tab();
    Gtk::Widget* build_providers_tab();
    Gtk::Widget* build_models_tab();
    Gtk::Widget* build_workspace_tab(); // Plan.md P6: types + prompt overrides
    Gtk::Widget* build_mcp_tab();
    Gtk::Widget* build_agent_tab();
    Gtk::Widget* build_web_search_tab(); // §WEB-SEARCH-CORE (2026-10-10)
    Gtk::Widget* build_chimpflow_tab(); // §CHIMPFLOW (2026-10-25)
    Gtk::Widget* build_search_tab();
    Gtk::Widget* build_paths_tab();

    void refresh_providers();
    void refresh_models();
    void refresh_prompts();
    void refresh_project_types();
    void refresh_mcp();
    void on_save();

    // ChimpFlow tab helpers
    ChimpFlowSettings chimpflow_from_widgets() const;
    void refresh_chimpflow_catalog();
    void preview_chimpflow_voice();

    // Agent tab helpers
    void toggle_skill(const std::string& name, bool enabled);
    void toggle_tool(const std::string& name, bool enabled);
    Gtk::Widget* make_toggle_row(const std::string& title,
                                 const std::string& subtitle, bool enabled,
                                 std::function<void(bool)> on_toggle);

    AppServices& services_;

    std::vector<Gtk::Button*> tab_buttons_;
    Gtk::Stack* stack_ = nullptr;
    Gtk::Label status_label_;

    // Providers tab
    Gtk::Box* providers_list_ = nullptr;
    // Models tab
    Gtk::DropDown* default_model_dropdown_ = nullptr;
    Gtk::Box* models_list_ = nullptr;
    bool updating_default_ = false; // guard re-entrancy while list is swapped
    // Interface tab
    Gtk::Label* workspace_label_ = nullptr;
    // Prompts tab
    Gtk::Box* prompts_list_ = nullptr;
    Gtk::Box* types_list_ = nullptr;
    sigc::signal<void(const std::string&)> signal_open_file_;
    // MCP tab
    Gtk::Box* mcp_list_ = nullptr;
    // Agent tab
    std::function<std::vector<daedalus::ToolInfo>()> tool_list_provider_;

    // ChimpFlow tab
    Gtk::Switch* chimpflow_enabled_ = nullptr;
    Gtk::Switch* chimpflow_read_assistant_ = nullptr;
    Gtk::Switch* chimpflow_dictation_ = nullptr;
    Gtk::Entry* chimpflow_server_ = nullptr;
    Gtk::SpinButton* chimpflow_port_ = nullptr;
    Gtk::PasswordEntry* chimpflow_key_ = nullptr;
    Gtk::Entry* chimpflow_stt_model_ = nullptr;
    Gtk::DropDown* chimpflow_tts_model_ = nullptr;
    Gtk::DropDown* chimpflow_voice_ = nullptr;
    Glib::RefPtr<Gtk::StringList> chimpflow_tts_items_;
    Glib::RefPtr<Gtk::StringList> chimpflow_voice_items_;
    Gtk::Button* chimpflow_refresh_ = nullptr;
    Gtk::Button* chimpflow_preview_ = nullptr;
    Gtk::Label* chimpflow_status_ = nullptr;
    bool updating_chimpflow_ = false;
    bool chimpflow_discovery_busy_ = false;
    bool chimpflow_preview_busy_ = false;
    std::uint64_t chimpflow_discovery_generation_ = 0;
    std::uint64_t chimpflow_preview_generation_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

    sigc::signal<void()> signal_theme_changed_;
    sigc::signal<void(const std::string&)> signal_workspace_changed_;
    sigc::signal<void()> signal_tree_refresh_;
    sigc::signal<void()> signal_agent_config_changed_;
};

} // namespace monkeybyte
