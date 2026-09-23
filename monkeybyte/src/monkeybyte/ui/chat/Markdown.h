#pragma once

#include <string>

namespace monkeybyte {

// §7.4 — lightweight markdown → Pango markup (no dependencies).
// Handles: fenced code blocks (with highlighting), inline `code`, **bold**,
// *italic*, - / * list items, line breaks. All input is Pango-escaped first.
class Markdown {
public:
    // Span colors used by the renderer. Pango span colors always beat CSS,
    // so these must follow the active theme explicitly (they are Catppuccin
    // Mocha/Latte pairs). They deliberately do NOT follow the "Assistant
    // Text" override from Settings → Chat — syntax accents are meant to
    // differ from plain text (§4.5).
    struct Palette {
        std::string comment;  // code comments
        std::string string;   // string literals
        std::string number;   // numeric literals
        std::string keyword;  // language keywords
        std::string code_bg;  // inline/fenced code background
    };

    static Palette dark_palette();  // Catppuccin Mocha (the default)
    static Palette light_palette(); // Catppuccin Latte

    // Process-wide palette, main thread only (chat rendering is main-thread).
    // Default is dark_palette(); ThemeManager::apply() keeps it in sync with
    // the resolved theme.
    static void set_palette(const Palette& p);
    static const Palette& palette();

    static std::string to_pango(const std::string& md);
};

// §7.4 — minimal tokenizer coloring keywords / strings / comments / numbers
// inside code spans. Input must already be Pango-escaped? No: pass raw code,
// output is escaped + span-wrapped. Uses Markdown::palette().
class Highlighter {
public:
    static std::string highlight(const std::string& code);
};

} // namespace monkeybyte
