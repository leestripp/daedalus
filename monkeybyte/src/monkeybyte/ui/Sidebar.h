#pragma once

#include "monkeybyte/FileOpenMode.h"
#include "monkeybyte/ui/ActivityBar.h"
#include <gtkmm.h>
#include <map>
#include <sigc++/sigc++.h>
#include <string>

namespace monkeybyte {

struct AppServices;
class WorkspacePanel;
class GitPanel;
class SettingsPanel;

// Hosts the four sidebar panels; one visible at a time (design §4).
class Sidebar : public Gtk::Box {
public:
    explicit Sidebar(AppServices& services);

    void show_panel(ActivityBar::Panel panel);
    void refresh_workspace();
    void reveal_in_tree(const std::string& path);

    sigc::signal<void(const std::string&, int)> signal_open_file() { return signal_open_file_; }
    sigc::signal<void(const std::string&, FileOpenMode)> signal_open_file_as() {
        return signal_open_file_as_;
    }
    sigc::signal<void(const std::string&)> signal_workspace_changed() {
        return signal_workspace_changed_;
    }
    // Forwarded from WorkspacePanel (Plan.md P6): workspace type changed;
    // MainWindow rebuilds the chat agent.
    sigc::signal<void()> signal_project_type_changed() {
        return signal_project_type_changed_;
    }
    sigc::signal<void(const std::string&)> signal_path_deleted() {
        return signal_path_deleted_;
    }
    sigc::signal<void(const std::string&, const std::string&)> signal_path_renamed() {
        return signal_path_renamed_;
    }
    // Agent-affecting config edited in Settings (MCP servers, skills,
    // tools, limits); MainWindow resets the chat agent.
    sigc::signal<void()> signal_agent_config_changed() {
        return signal_agent_config_changed_;
    }

    // Gives MainWindow access for wiring (tool list provider).
    SettingsPanel* settings_panel() const { return settings_panel_; }

private:
    AppServices& services_;
    std::map<ActivityBar::Panel, Gtk::Widget*> panels_;
    WorkspacePanel* workspace_panel_ = nullptr;
    GitPanel* git_panel_ = nullptr;
    SettingsPanel* settings_panel_ = nullptr;
    sigc::signal<void(const std::string&, int)> signal_open_file_;
    sigc::signal<void(const std::string&, FileOpenMode)> signal_open_file_as_;
    sigc::signal<void(const std::string&)> signal_workspace_changed_;
    sigc::signal<void()> signal_project_type_changed_;
    sigc::signal<void(const std::string&)> signal_path_deleted_;
    sigc::signal<void(const std::string&, const std::string&)> signal_path_renamed_;
    sigc::signal<void()> signal_agent_config_changed_;
};

} // namespace monkeybyte
