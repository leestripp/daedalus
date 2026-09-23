#include "monkeybyte/ui/Sidebar.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/ui/panels/GitPanel.h"
#include "monkeybyte/ui/panels/SearchPanel.h"
#include "monkeybyte/ui/panels/SettingsPanel.h"
#include "monkeybyte/ui/panels/WorkspacePanel.h"

namespace monkeybyte {

Sidebar::Sidebar(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL), services_(services) {
    add_css_class("sidebar");

    auto* workspace = Gtk::make_managed<WorkspacePanel>(services_);
    auto* search = Gtk::make_managed<SearchPanel>(services_);
    auto* git = Gtk::make_managed<GitPanel>(services_);
    auto* settings = Gtk::make_managed<SettingsPanel>(services_);

    for (auto* panel : {static_cast<Gtk::Widget*>(workspace),
                        static_cast<Gtk::Widget*>(search),
                        static_cast<Gtk::Widget*>(git),
                        static_cast<Gtk::Widget*>(settings)}) {
        panel->set_visible(false);
        append(*panel);
    }

    workspace->signal_open_file().connect(
        [this](const std::string& p, int line) { signal_open_file_.emit(p, line); });
    workspace->signal_open_file_as().connect(
        [this](const std::string& p, FileOpenMode mode) {
            signal_open_file_as_.emit(p, mode);
        });
    workspace->signal_workspace_changed().connect(
        [this](const std::string& p) { signal_workspace_changed_.emit(p); });
    workspace->signal_project_type_changed().connect(
        [this] { signal_project_type_changed_.emit(); });
    workspace->signal_path_deleted().connect(
        [this](const std::string& p) { signal_path_deleted_.emit(p); });
    workspace->signal_path_renamed().connect(
        [this](const std::string& old_p, const std::string& new_p) {
            signal_path_renamed_.emit(old_p, new_p);
        });
    search->signal_open_file().connect(
        [this](const std::string& p, int line) { signal_open_file_.emit(p, line); });
    git->signal_open_file().connect(
        [this](const std::string& p, int line) { signal_open_file_.emit(p, line); });
    settings->signal_theme_changed().connect([this] { /* live, no restart */ });
    settings->signal_workspace_changed().connect(
        [this](const std::string& p) { signal_workspace_changed_.emit(p); });
    settings->signal_open_file().connect(
        [this](const std::string& p) { signal_open_file_.emit(p, 0); });
    settings->signal_tree_refresh().connect(
        [this] { refresh_workspace(); });
    settings->signal_agent_config_changed().connect(
        [this] { signal_agent_config_changed_.emit(); });

    panels_[ActivityBar::Workspace] = workspace;
    panels_[ActivityBar::Search] = search;
    panels_[ActivityBar::Git] = git;
    panels_[ActivityBar::Settings] = settings;
    git_panel_ = git;
    workspace_panel_ = workspace;
    settings_panel_ = settings;

    show_panel(static_cast<ActivityBar::Panel>(
        services_.sidebar->get_int("active-panel")));
}

void Sidebar::show_panel(ActivityBar::Panel panel) {
    for (auto& [p, widget] : panels_)
        widget->set_visible(p == panel);
    if (panel == ActivityBar::Git && git_panel_)
        git_panel_->refresh(); // refresh on each open (§4.4)
}

void Sidebar::refresh_workspace() {
    if (workspace_panel_) workspace_panel_->refresh();
    if (git_panel_) git_panel_->refresh();
}

void Sidebar::reveal_in_tree(const std::string& path) {
    if (workspace_panel_) workspace_panel_->reveal(path);
}

} // namespace monkeybyte
