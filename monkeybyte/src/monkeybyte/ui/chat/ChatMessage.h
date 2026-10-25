#pragma once

#include <gtkmm.h>
#include <string>

namespace monkeybyte {

// §7.3 — one chat bubble. Assistant bodies are re-rendered from markdown as
// text streams in; a blinking caret shows while streaming.
class ChatMessage : public Gtk::Box {
public:
    enum class Role { User, Assistant };

    explicit ChatMessage(Role role);
    ~ChatMessage() override;

    // §MSG-HEADERS (2026-10-04) — origin header for messages that did
    // not come from the conversation's own participants: `source` is the
    // `_meta.source` string ("user:<name>", "a2a:<peer>",
    // "trigger:<name>", "system:context-status", …), `received_at` its
    // "_meta.received_at" timestamp ("" when unknown). No-op for origins
    // that need no header (assistant's own voice, legacy unstamped).
    // Call any time — the label is prepended, so ordering vs set_text
    // does not matter.
    void set_origin(const std::string& source, const std::string& received_at);

    // user: plain text + image thumbnails; assistant: markdown.
    // §READ-IMAGE — user text is scanned for ![alt](path) attachment
    // references (the attach-button syntax); existing local files render
    // as thumbnails above the text, the markup itself is hidden.
    void set_text(const std::string& text);
    void append_text(const std::string& delta); // streaming assistant
    void finalize(); // stop the streaming caret
    bool has_text() const { return !raw_.empty(); }

private:
    void show_copy_menu(double x, double y);
    void render(const std::string& pango); // markup, crash-guarded
    void show_user_text(const std::string& text); // §READ-IMAGE

    Role role_;
    std::string raw_; // accumulated assistant markdown (or plain user text)
    Gtk::Label origin_label_; // §MSG-HEADERS — header, hidden when no origin
    Gtk::Box images_; // §READ-IMAGE — thumbnails, hidden when empty
    Gtk::Label label_;
    bool streaming_ = false;
    bool caret_on_ = false;
    sigc::connection caret_timer_;
};

} // namespace monkeybyte
