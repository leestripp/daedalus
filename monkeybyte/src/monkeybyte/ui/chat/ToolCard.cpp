#include "monkeybyte/ui/chat/ToolCard.h"

#include <algorithm>
#include <cctype>

namespace monkeybyte {

namespace {

// daedalus sanitises live bash output for TTYs but preserves ANSI SGR
// colour/style sequences — strip those for Gtk::Label display.
std::string strip_sgr(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < in.size() && in[j] != 'm') ++j;
            if (j < in.size()) { i = j; continue; } // complete SGR sequence
        }
        out += in[i];
    }
    return out;
}

} // namespace

// Symbolic header icon per tool kind (design §7.3). daedalus built-ins and
// MCP tools are matched by name substring; unknown tools get the wrench.
std::string tool_icon_name(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto has = [&](const std::string& sub) { return lower.find(sub) != std::string::npos; };
    if (has("bash") || has("shell") || has("terminal") || has("command"))
        return "mb-tool-terminal-symbolic";
    if (has("read") || has("write") || has("edit") || has("file") || has("patch"))
        return "mb-tool-file-symbolic";
    if (has("search") || has("grep") || has("glob") || has("find") || has("list"))
        return "mb-tool-search-symbolic";
    if (has("http") || has("fetch") || has("web") || has("browser") || has("url"))
        return "mb-globe-symbolic";
    return "mb-tool-symbolic";
}

ToolCard::ToolCard(const ToolInfo& info) : Gtk::Box(Gtk::Orientation::VERTICAL) {
    add_css_class("tool-card");
    set_margin_top(4);
    set_margin_bottom(4);
    set_margin_start(8);
    set_margin_end(8);

    header_.set_has_frame(false);
    auto* header_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
    tool_icon_.set_pixel_size(14);
    name_label_.set_halign(Gtk::Align::START);
    name_label_.set_xalign(0.0f);
    name_label_.set_hexpand(true);
    name_label_.set_ellipsize(Pango::EllipsizeMode::END);
    status_spinner_.set_size_request(12, 12);
    status_icon_.set_pixel_size(12);
    status_icon_.add_css_class("tool-status");
    status_label_.add_css_class("muted");
    chevron_.set_pixel_size(12);
    header_box->append(tool_icon_);
    header_box->append(name_label_);
    header_box->append(status_spinner_);
    header_box->append(status_icon_);
    header_box->append(status_label_);
    header_box->append(chevron_);
    header_.set_child(*header_box);
    header_.signal_clicked().connect([this] {
        expanded_ = !expanded_;
        revealer_.set_reveal_child(expanded_);
        chevron_.set_from_icon_name(expanded_ ? "mb-chevron-down-symbolic"
                                              : "mb-chevron-right-symbolic");
    });
    append(header_);

    body_.add_css_class("monospace");
    body_.set_halign(Gtk::Align::FILL); // wrap at the real card width
    body_.set_xalign(0.0f);
    body_.set_wrap(true);
    body_.set_wrap_mode(Pango::WrapMode::WORD_CHAR); // break long tokens/paths
    body_.set_selectable(true);
    body_.set_margin(8);
    revealer_.set_child(body_);
    append(revealer_);

    update(info);
}

void ToolCard::update(const ToolInfo& info) {
    if (!info.name.empty()) { // progress updates carry no name
        name_ = info.name;
        name_label_.set_text(name_);
        tool_icon_.set_from_icon_name(tool_icon_name(name_));
    }
    status_label_.set_text(info.status);
    chevron_.set_from_icon_name(expanded_ ? "mb-chevron-down-symbolic"
                                          : "mb-chevron-right-symbolic");

    remove_css_class("success");
    remove_css_class("error");
    if (info.done) {
        status_spinner_.stop();
        status_spinner_.set_visible(false);
        status_icon_.set_from_icon_name(info.success ? "mb-status-ok-symbolic"
                                                     : "mb-status-error-symbolic");
        status_icon_.set_visible(true);
        add_css_class(info.success ? "success" : "error");
        body_.set_text(info.result);
    } else {
        status_icon_.set_visible(false);
        status_spinner_.set_visible(true);
        status_spinner_.start();
        if (!info.live_output.empty()) {
            // §7.3 — live command output while the tool runs: show the tail,
            // cut at a UTF-8 boundary so no sequence is split.
            constexpr std::size_t kShow = 2048;
            std::string tail = info.live_output.size() > kShow
                                   ? info.live_output.substr(info.live_output.size() - kShow)
                                   : info.live_output;
            std::size_t start = 0;
            while (start < tail.size() &&
                   (static_cast<unsigned char>(tail[start]) & 0xC0) == 0x80)
                ++start;
            body_.set_text(strip_sgr(tail.substr(start)));
        } else if (!info.args.empty()) {
            body_.set_text(info.args);
        }
    }
}

} // namespace monkeybyte
