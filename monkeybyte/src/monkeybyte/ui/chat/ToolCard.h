#pragma once

#include "monkeybyte/ui/chat/ChatController.h"

#include <gtkmm.h>

namespace monkeybyte {

// §7.3 — collapsible tool card: header (tool icon + name + status + chevron)
// + revealer body. Status shows a spinner while running, then a ✓/✗ icon.
class ToolCard : public Gtk::Box {
public:
    explicit ToolCard(const ToolInfo& info);

    void update(const ToolInfo& info); // progress / done

private:
    Gtk::Button header_;
    Gtk::Image tool_icon_;
    Gtk::Label name_label_;
    Gtk::Spinner status_spinner_;
    Gtk::Image status_icon_;
    Gtk::Label status_label_;
    Gtk::Image chevron_;
    Gtk::Revealer revealer_;
    Gtk::Label body_;
    bool expanded_ = false;
    std::string name_; // progress updates carry no name; keep the first one
};

} // namespace monkeybyte
