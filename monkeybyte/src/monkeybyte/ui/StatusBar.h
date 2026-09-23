#pragma once

#include <daedalus/types/AgentStats.h>

#include <gtkmm.h>
#include <string>

namespace monkeybyte {

// Bottom 24 px accent bar (design §8).
class StatusBar : public Gtk::Box {
public:
    StatusBar();

    void set_branch_info(const std::string& text); // "" hides the left label
    void set_model(const std::string& model);
    // Live daedalus stats; zero stats (no session) hides the stat segments.
    void set_agent_stats(const daedalus::AgentStats& stats);

private:
    void refresh_model_label();

    Gtk::Label branch_label_;
    Gtk::Label model_label_;
    std::string model_;
    daedalus::AgentStats stats_;
};

} // namespace monkeybyte
