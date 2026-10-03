#include "monkeybyte/ui/chat/ChatMessage.h"

#include "monkeybyte/ui/chat/Markdown.h"

#include <algorithm>
#include <filesystem>
#include <vector>

namespace monkeybyte {

namespace {

// Markdown rendering must never crash the app: GLib aborts when a C++
// exception escapes a signal handler (stream deltas render inside timeout
// callbacks), so a renderer bug degrades to escaped plain text instead.
std::string safe_pango(const std::string& raw) {
    try {
        return Markdown::to_pango(raw);
    } catch (const std::exception&) {
        return Glib::Markup::escape_text(raw);
    }
}

// §READ-IMAGE — one ![alt](path) attachment reference in user text.
// Same bracket-scanning shape as daedalus's find_image_references, but
// only existing local files are lifted out as thumbnails; anything else
// (URLs, missing files) stays in the text untouched.
struct ImageRef {
    std::string path;
    size_t start; // of "!["
    size_t end;   // one past ")"
};

std::vector<ImageRef> find_local_images(const std::string& text) {
    std::vector<ImageRef> refs;
    size_t pos = 0;
    while (true) {
        const size_t start = text.find("![", pos);
        if (start == std::string::npos) break;
        const size_t link_start = text.find("](", start);
        if (link_start == std::string::npos) break;
        const size_t link_end = text.find(")", link_start + 2);
        if (link_end == std::string::npos) break;
        std::string path =
            text.substr(link_start + 2, link_end - link_start - 2);
        const size_t first = path.find_first_not_of(" \t");
        const size_t last = path.find_last_not_of(" \t");
        if (first != std::string::npos) {
            path = path.substr(first, last - first + 1);
            std::error_code ec;
            if (std::filesystem::is_regular_file(path, ec))
                refs.push_back({path, start, link_end + 1});
        }
        pos = link_end + 1;
    }
    return refs;
}

// Longest-edge cap for chat thumbnails (px). Textures load at full size;
// the Picture scales down for display.
constexpr int kThumbMax = 320;

// §MSG-HEADERS — human label for a message's `_meta.source` ("kind:id").
// Returns "" for origins that need no header: the assistant's own voice,
// bare system messages, and legacy messages persisted before headers
// existed. The kind-specific wording is the whole point of the envelope —
// an A2A peer or a trigger must not read like a fresh interactive prompt.
std::string origin_label(const std::string& source,
                         const std::string& received_at) {
    std::string label;
    const auto colon = source.find(':');
    const std::string kind = source.substr(0, colon);
    const std::string id =
        colon == std::string::npos ? "" : source.substr(colon + 1);
    if (source == "system:context-status") {
        label = "Context status";
    } else if (source.empty() || source == "assistant" || source == "system") {
        return ""; // no header for the conversation's own voices
    } else if (kind == "user") {
        label = "You"; // "user" or "user:<name>" — this host's own user
    } else if (kind == "a2a") {
        label = "From " + id + " (A2A)";
    } else if (kind == "trigger") {
        label = "Trigger: " + id;
    } else if (!id.empty()) {
        label = "From " + id + " (" + kind + ")";
    } else {
        label = source; // unknown bare kind — show it verbatim
    }
    if (!received_at.empty()) label += " · " + received_at;
    return label;
}

} // namespace

// set_markup() itself throws Glib::Error on unparseable markup/invalid UTF-8
// (agent output embeds arbitrary bytes, e.g. raw HTTP bodies) — and several
// call sites are Glib timeout/idle callbacks where an escape aborts the
// process. Degrade to plain escaped text instead.
void ChatMessage::render(const std::string& pango) {
    try {
        label_.set_markup(pango);
    } catch (const std::exception&) {
        label_.set_text(raw_);
    }
}

ChatMessage::~ChatMessage() {
    // clear_history() can drop a bubble mid-stream; without this the 500 ms
    // caret timeout would fire on a destroyed widget (use-after-free).
    caret_timer_.disconnect();
}

ChatMessage::ChatMessage(Role role)
    : Gtk::Box(Gtk::Orientation::VERTICAL), role_(role) {
    add_css_class(role == Role::User ? "message-user" : "message-assistant");
    set_margin_top(4);
    set_margin_bottom(4);
    set_margin_start(8);
    set_margin_end(8);

    label_.set_halign(Gtk::Align::FILL); // wrap at the real bubble width,
    label_.set_wrap(true);               // not the content requisition
    label_.set_wrap_mode(Pango::WrapMode::WORD_CHAR); // break long code tokens
    label_.set_selectable(true);
    label_.set_xalign(0.0f);

    // §MSG-HEADERS — origin header sits above thumbnails and body.
    origin_label_.set_visible(false); // shown on demand by set_origin()
    origin_label_.set_xalign(0.0f);
    origin_label_.add_css_class("message-origin");
    prepend(origin_label_);

    images_.set_orientation(Gtk::Orientation::VERTICAL);
    images_.set_spacing(4);
    images_.set_visible(false); // shown on demand by show_user_text()
    append(images_);
    append(label_);

    // right-click → Copy Text (§7.3)
    auto menu_click = Gtk::GestureClick::create();
    menu_click->set_button(GDK_BUTTON_SECONDARY);
    menu_click->signal_pressed().connect(
        [this](int, double x, double y) { show_copy_menu(x, y); });
    add_controller(menu_click);
}

void ChatMessage::show_copy_menu(double x, double y) {
    auto menu = Gio::Menu::create();
    auto group = Gio::SimpleActionGroup::create();
    menu->append("Copy Text", "msg.copy");
    group->add_action("copy", [this] {
        get_clipboard()->set_text(raw_);
    });
    insert_action_group("msg", group);
    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_parent(*this);
    popover->set_has_arrow(false);
    popover->set_pointing_to(Gdk::Rectangle(
        static_cast<int>(x), static_cast<int>(y), 1, 1));
    popover->popup();
}

void ChatMessage::set_origin(const std::string& source,
                             const std::string& received_at) {
    const std::string label = origin_label(source, received_at);
    if (label.empty()) return; // this origin needs no header
    origin_label_.set_text(label);
    origin_label_.set_visible(true);
}

void ChatMessage::set_text(const std::string& text) {
    raw_ = text;
    if (role_ == Role::User) {
        show_user_text(text); // plain + thumbnails, newlines preserved (§7.3)
    } else {
        render(safe_pango(raw_));
    }
}

// §READ-IMAGE — lift ![alt](path) attachments out of the visible text and
// show them as thumbnails above it. A bubble that is only attachments
// shows no text label at all.
void ChatMessage::show_user_text(const std::string& text) {
    // Rebuild from scratch: set_text is the single entry point for both
    // live sends and history restore.
    while (auto* child = images_.get_first_child()) images_.remove(*child);
    images_.set_visible(false);
    label_.set_visible(true);

    const auto refs = find_local_images(text);
    std::string display = text;
    // Erase back-to-front so earlier offsets stay valid; thumbnails are
    // collected and appended afterwards so they keep document order.
    std::vector<Gtk::Picture*> pictures;
    for (auto it = refs.rbegin(); it != refs.rend(); ++it) {
        Glib::RefPtr<Gdk::Texture> texture;
        try {
            texture =
                Gdk::Texture::create_from_file(Gio::File::create_for_path(it->path));
        } catch (const Glib::Error&) {
            continue; // unreadable/corrupt: leave the markup in the text
        }
        display.erase(it->start, it->end - it->start);

        auto* pic = Gtk::make_managed<Gtk::Picture>();
        const int w = texture->get_width();
        const int h = texture->get_height();
        if (w > 0 && h > 0) {
            const double scale =
                std::min(1.0, kThumbMax / static_cast<double>(std::max(w, h)));
            pic->set_size_request(static_cast<int>(w * scale),
                                  static_cast<int>(h * scale));
        }
        pic->set_paintable(texture);
        pic->set_content_fit(Gtk::ContentFit::CONTAIN);
        pic->set_can_shrink(true);
        pic->set_halign(Gtk::Align::START);
        pic->add_css_class("message-image");
        pic->set_tooltip_text(it->path);
        pictures.push_back(pic);
    }
    for (auto it = pictures.rbegin(); it != pictures.rend(); ++it) {
        images_.append(**it);
        images_.set_visible(true);
    }

    // Drop lines left blank by erased references, then surrounding space.
    std::string cleaned;
    size_t line_start = 0;
    while (line_start <= display.size()) {
        const size_t nl = display.find('\n', line_start);
        const std::string line = display.substr(
            line_start, nl == std::string::npos ? nl : nl - line_start);
        if (line.find_first_not_of(" \t\r") != std::string::npos) {
            if (!cleaned.empty()) cleaned += '\n';
            cleaned += line;
        }
        if (nl == std::string::npos) break;
        line_start = nl + 1;
    }

    if (cleaned.empty()) {
        label_.set_visible(false); // attachments only
    } else {
        label_.set_text(cleaned);
    }
}

void ChatMessage::append_text(const std::string& delta) {
    raw_ += delta;
    if (!streaming_) {
        streaming_ = true;
        // While streaming, set_markup() recreates the label's Pango layout on
        // every delta and caret blink. A selectable label clicked mid-swap can
        // hit GTK's selection code with a null layout
        // (gdk_pango_layout_get_clip_region assertion → crash), so selection
        // is only enabled again in finalize().
        label_.set_selectable(false);
        caret_timer_ = Glib::signal_timeout().connect(
            [this] {
                caret_on_ = !caret_on_;
                render(safe_pango(raw_) + (caret_on_ ? " ▌" : ""));
                return streaming_; // stop when finalized
            },
            500);
    }
    render(safe_pango(raw_) + " ▌");
}

void ChatMessage::finalize() {
    streaming_ = false;
    caret_timer_.disconnect();
    render(safe_pango(raw_));
    label_.set_selectable(true); // layout is stable again — see append_text()
}

} // namespace monkeybyte
