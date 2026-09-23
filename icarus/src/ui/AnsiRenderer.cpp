// SPDX-License-Identifier: BSL-1.0
// src/ui/AnsiRenderer.cpp

#include "AnsiRenderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <sstream>
#include <unordered_set>

#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#endif

namespace icarus::ui {

// ---------------------------------------------------------------------------
// Internal ANSI constants.
// ---------------------------------------------------------------------------
constexpr std::string_view kReset = "\x1b[0m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kItalic = "\x1b[3m";

// Semantic color palette (256-color codes).
constexpr std::uint8_t kPrimary = 39;  // cyan
constexpr std::uint8_t kSuccess = 76;  // green
constexpr std::uint8_t kWarning = 220; // yellow
constexpr std::uint8_t kError = 196;   // red
constexpr std::uint8_t kInfo = 75;     // blue
constexpr std::uint8_t kMuted = 245;   // gray

// ---------------------------------------------------------------------------
// Box-drawing characters.
// ---------------------------------------------------------------------------
// Convert a Unicode box char to UTF-8 bytes.
std::string to_utf8(char32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

std::string box_char(char32_t cp, bool unicode) {
    if (!unicode) {
        // Map back to ASCII equivalents.
        switch (cp) {
            case u'─': return "-";
            case u'│': return "|";
            case u'╔': case u'╗': case u'╚': case u'╝':
            case u'╡': case u'╞':
            case u'╭': case u'╮': case u'╰': case u'╯':
            case u'┬': case u'┴': case u'├': case u'┤': case u'┼':
                return "+";
            default: return "?";
        }
    }
    return to_utf8(cp);
}

namespace {

// ---------------------------------------------------------------------------
// Environment helpers.
// ---------------------------------------------------------------------------
bool env_is_set(const char* name) {
    const char* v = std::getenv(name);
    return v && v[0] != '\0';
}

int default_terminal_width() {
#ifdef __linux__
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
#endif
    const char* c = std::getenv("COLUMNS");
    if (c) {
        try { return std::stoi(c); } catch (...) {}
    }
    return 80;
}

// ---------------------------------------------------------------------------
// Code highlighting keyword sets.
// ---------------------------------------------------------------------------
std::unordered_set<std::string_view> cpp_keywords() {
    return {
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand",
        "bitor", "bool", "break", "case", "catch", "char", "char8_t",
        "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "float", "for", "friend", "goto", "if", "inline",
        "int", "long", "mutable", "namespace", "new", "noexcept", "not",
        "not_eq", "nullptr", "operator", "or", "or_eq", "private",
        "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t",
        "while", "xor", "xor_eq"
    };
}

std::unordered_set<std::string_view> python_keywords() {
    return {
        "and", "as", "assert", "async", "await", "break", "class",
        "continue", "def", "del", "elif", "else", "except", "False",
        "finally", "for", "from", "global", "if", "import", "in", "is",
        "lambda", "None", "nonlocal", "not", "or", "pass", "raise",
        "return", "True", "try", "while", "with", "yield"
    };
}

std::unordered_set<std::string_view> js_keywords() {
    return {
        "async", "await", "break", "case", "catch", "class", "const",
        "continue", "debugger", "default", "delete", "do", "else",
        "export", "extends", "false", "finally", "for", "function",
        "if", "import", "in", "instanceof", "let", "new", "null",
        "return", "super", "switch", "this", "throw", "true", "try",
        "typeof", "var", "void", "while", "with", "yield"
    };
}

const std::unordered_set<std::string_view>& keywords_for(std::string_view lang) {
    static const auto cpp = cpp_keywords();
    static const auto py = python_keywords();
    static const auto js = js_keywords();
    if (lang == "cpp" || lang == "c++" || lang == "cxx" ||
        lang == "c" || lang == "h" || lang == "hpp") return cpp;
    if (lang == "python" || lang == "py") return py;
    if (lang == "javascript" || lang == "js" || lang == "typescript" ||
        lang == "ts") return js;
    return cpp; // default
}

} // namespace

// ---------------------------------------------------------------------------
// Live width query (resize-aware)
// ---------------------------------------------------------------------------
int effective_terminal_width(const RenderConfig& cfg) {
#ifdef __linux__
    struct winsize ws{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return static_cast<int>(ws.ws_col);
    }
#endif
    const char* c = std::getenv("COLUMNS");
    if (c) {
        try {
            int v = std::stoi(c);
            if (v > 0) return v;
        } catch (...) {}
    }
    return cfg.terminal_width;
}

// ---------------------------------------------------------------------------
// RenderConfig
// ---------------------------------------------------------------------------
RenderConfig RenderConfig::detect(bool force_no_color, bool force_no_unicode, bool force_no_hud) {
    RenderConfig cfg;
    cfg.terminal_width = default_terminal_width();
    // Use the real terminal width as detected (ioctl TIOCGWINSZ, then
    // $COLUMNS, then 80 as last-resort fallback).

    bool tty = ::isatty(STDOUT_FILENO) != 0;
    bool no_color_env = env_is_set("NO_COLOR");
    cfg.color_enabled = tty && !force_no_color && !no_color_env;

    // Rich output is the default. Only --no-unicode / --plain disables it.
    // We deliberately do not gate Unicode on LC_ALL/TERM: modern terminals
    // handle UTF-8 box drawing well, and falling back to ASCII by default
    // makes icarus look worse than it is.
    cfg.unicode_enabled = !force_no_unicode;
    cfg.hud_enabled = !force_no_hud;
    return cfg;
}

// ---------------------------------------------------------------------------
// Style
// ---------------------------------------------------------------------------
std::string Style::reset() const { return cfg_.color_enabled ? std::string(kReset) : std::string(); }

std::string Style::wrap(std::string_view text,
                        std::string_view prefix,
                        std::string_view suffix) const {
    if (!cfg_.color_enabled) return std::string(text);
    std::string out;
    out.reserve(prefix.size() + text.size() + suffix.size());
    out.append(prefix);
    out.append(text);
    out.append(suffix);
    return out;
}

std::string Style::bold(std::string_view text) const { return wrap(text, kBold, kReset); }
std::string Style::dim(std::string_view text) const { return wrap(text, kDim, kReset); }
std::string Style::italic(std::string_view text) const { return wrap(text, kItalic, kReset); }

std::string Style::primary(std::string_view text) const { return fg256(text, kPrimary); }
std::string Style::success(std::string_view text) const { return fg256(text, kSuccess); }
std::string Style::warning(std::string_view text) const { return fg256(text, kWarning); }
std::string Style::error(std::string_view text) const { return fg256(text, kError); }
std::string Style::info(std::string_view text) const { return fg256(text, kInfo); }
std::string Style::muted(std::string_view text) const { return fg256(text, kMuted); }

std::string Style::fg256(std::string_view text, std::uint8_t color) const {
    if (!cfg_.color_enabled) return std::string(text);
    std::ostringstream oss;
    oss << "\x1b[38;5;" << static_cast<int>(color) << "m" << text << kReset;
    return oss.str();
}

std::string Style::rgb(std::string_view text,
                       std::uint8_t r,
                       std::uint8_t g,
                       std::uint8_t b) const {
    if (!cfg_.color_enabled) return std::string(text);
    std::ostringstream oss;
    oss << "\x1b[38;2;" << static_cast<int>(r) << ";"
        << static_cast<int>(g) << ";" << static_cast<int>(b) << "m"
        << text << kReset;
    return oss.str();
}

// ---------------------------------------------------------------------------
// Text utilities.
// ---------------------------------------------------------------------------
namespace {

// Markus Kuhn public-domain wcwidth algorithm.
// See: https://www.cl.cam.ac.uk/~mgk25/ucs/wcwidth.c
// This is the non-CJK variant: ambiguous-width characters (box drawing,
// Greek, Cyrillic, etc.) are treated as single-width, matching modern
// Western terminal emulators.

struct Interval {
    char32_t first;
    char32_t last;
};

constexpr bool bisearch(char32_t ucs, const Interval* table, int max) {
    int min = 0;
    while (max >= min) {
        int mid = (min + max) / 2;
        if (ucs > table[mid].last) {
            min = mid + 1;
        } else if (ucs < table[mid].first) {
            max = mid - 1;
        } else {
            return true;
        }
    }
    return false;
}

// Display width of a single Unicode codepoint in a monospace terminal.
// Returns 0 for combining/control characters, 2 for CJK/fullwidth, 1 otherwise.
int codepoint_display_width(char32_t ucs) {
    // sorted list of non-overlapping intervals of non-spacing characters
    static constexpr Interval combining[] = {
        { 0x0300, 0x036F }, { 0x0483, 0x0486 }, { 0x0488, 0x0489 },
        { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 },
        { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 }, { 0x0600, 0x0603 },
        { 0x0610, 0x0615 }, { 0x064B, 0x065E }, { 0x0670, 0x0670 },
        { 0x06D6, 0x06E4 }, { 0x06E7, 0x06E8 }, { 0x06EA, 0x06ED },
        { 0x070F, 0x070F }, { 0x0711, 0x0711 }, { 0x0730, 0x074A },
        { 0x07A6, 0x07B0 }, { 0x07EB, 0x07F3 }, { 0x0901, 0x0902 },
        { 0x093C, 0x093C }, { 0x0941, 0x0948 }, { 0x094D, 0x094D },
        { 0x0951, 0x0954 }, { 0x0962, 0x0963 }, { 0x0981, 0x0981 },
        { 0x09BC, 0x09BC }, { 0x09C1, 0x09C4 }, { 0x09CD, 0x09CD },
        { 0x09E2, 0x09E3 }, { 0x0A01, 0x0A02 }, { 0x0A3C, 0x0A3C },
        { 0x0A41, 0x0A42 }, { 0x0A47, 0x0A48 }, { 0x0A4B, 0x0A4D },
        { 0x0A70, 0x0A71 }, { 0x0A81, 0x0A82 }, { 0x0ABC, 0x0ABC },
        { 0x0AC1, 0x0AC5 }, { 0x0AC7, 0x0AC8 }, { 0x0ACD, 0x0ACD },
        { 0x0AE2, 0x0AE3 }, { 0x0B01, 0x0B01 }, { 0x0B3C, 0x0B3C },
        { 0x0B3F, 0x0B3F }, { 0x0B41, 0x0B43 }, { 0x0B4D, 0x0B4D },
        { 0x0B56, 0x0B56 }, { 0x0B82, 0x0B82 }, { 0x0BC0, 0x0BC0 },
        { 0x0BCD, 0x0BCD }, { 0x0C3E, 0x0C40 }, { 0x0C46, 0x0C48 },
        { 0x0C4A, 0x0C4D }, { 0x0C55, 0x0C56 }, { 0x0CBC, 0x0CBC },
        { 0x0CBF, 0x0CBF }, { 0x0CC6, 0x0CC6 }, { 0x0CCC, 0x0CCD },
        { 0x0CE2, 0x0CE3 }, { 0x0D41, 0x0D43 }, { 0x0D4D, 0x0D4D },
        { 0x0DCA, 0x0DCA }, { 0x0DD2, 0x0DD4 }, { 0x0DD6, 0x0DD6 },
        { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A }, { 0x0E47, 0x0E4E },
        { 0x0EB1, 0x0EB1 }, { 0x0EB4, 0x0EB9 }, { 0x0EBB, 0x0EBC },
        { 0x0EC8, 0x0ECD }, { 0x0F18, 0x0F19 }, { 0x0F35, 0x0F35 },
        { 0x0F37, 0x0F37 }, { 0x0F39, 0x0F39 }, { 0x0F71, 0x0F7E },
        { 0x0F80, 0x0F84 }, { 0x0F86, 0x0F87 }, { 0x0F90, 0x0F97 },
        { 0x0F99, 0x0FBC }, { 0x0FC6, 0x0FC6 }, { 0x102D, 0x1030 },
        { 0x1032, 0x1032 }, { 0x1036, 0x1037 }, { 0x1039, 0x1039 },
        { 0x1058, 0x1059 }, { 0x1160, 0x11FF }, { 0x135F, 0x135F },
        { 0x1712, 0x1714 }, { 0x1732, 0x1734 }, { 0x1752, 0x1753 },
        { 0x1772, 0x1773 }, { 0x17B4, 0x17B5 }, { 0x17B7, 0x17BD },
        { 0x17C6, 0x17C6 }, { 0x17C9, 0x17D3 }, { 0x17DD, 0x17DD },
        { 0x180B, 0x180D }, { 0x18A9, 0x18A9 }, { 0x1920, 0x1922 },
        { 0x1927, 0x1928 }, { 0x1932, 0x1932 }, { 0x1939, 0x193B },
        { 0x1A17, 0x1A18 }, { 0x1B00, 0x1B03 }, { 0x1B34, 0x1B34 },
        { 0x1B36, 0x1B3A }, { 0x1B3C, 0x1B3C }, { 0x1B42, 0x1B42 },
        { 0x1B6B, 0x1B73 }, { 0x1DC0, 0x1DCA }, { 0x1DFE, 0x1DFF },
        { 0x200B, 0x200F }, { 0x202A, 0x202E }, { 0x2060, 0x2063 },
        { 0x206A, 0x206F }, { 0x20D0, 0x20EF }, { 0x302A, 0x302F },
        { 0x3099, 0x309A }, { 0xA806, 0xA806 }, { 0xA80B, 0xA80B },
        { 0xA825, 0xA826 }, { 0xFB1E, 0xFB1E }, { 0xFE00, 0xFE0F },
        { 0xFE20, 0xFE23 }, { 0xFEFF, 0xFEFF }, { 0xFFF9, 0xFFFB },
        { 0x10A01, 0x10A03 }, { 0x10A05, 0x10A06 }, { 0x10A0C, 0x10A0F },
        { 0x10A38, 0x10A3A }, { 0x10A3F, 0x10A3F }, { 0x1D167, 0x1D169 },
        { 0x1D173, 0x1D182 }, { 0x1D185, 0x1D18B }, { 0x1D1AA, 0x1D1AD },
        { 0x1D242, 0x1D244 }, { 0xE0001, 0xE0001 }, { 0xE0020, 0xE007F },
        { 0xE0100, 0xE01EF }
    };

    if (ucs == 0) return 0;
    if (ucs < 32 || (ucs >= 0x7f && ucs < 0xa0)) return 0;
    if (bisearch(ucs, combining, static_cast<int>(sizeof(combining) / sizeof(combining[0])) - 1)) return 0;

    // Wide emoji/symbol blocks that Kuhn's Unicode-5 table does not cover.
    bool wide_emoji =
        (ucs >= 0x2600 && ucs <= 0x27bf) ||     // Misc Symbols, Dingbats
        (ucs >= 0x1f300 && ucs <= 0x1f3fa) ||   // Misc Symbols and Pictographs
        (ucs >= 0x1f400 && ucs <= 0x1f64f) ||   // Emoticons, people, faces
        (ucs >= 0x1f680 && ucs <= 0x1f6ff) ||   // Transport and Map
        (ucs >= 0x1f700 && ucs <= 0x1f77f) ||   // Alchemical
        (ucs >= 0x1f780 && ucs <= 0x1f7ff) ||   // Geometric Shapes Extended
        (ucs >= 0x1f800 && ucs <= 0x1f8ff) ||   // Supplemental Arrows-C
        (ucs >= 0x1f900 && ucs <= 0x1f9ff) ||   // Supplemental Symbols/Pictographs
        (ucs >= 0x1fa00 && ucs <= 0x1fa6f) ||   // Chess Symbols
        (ucs >= 0x1fa70 && ucs <= 0x1faff) ||   // Symbols and Pictographs Extended-A
        (ucs >= 0x2300 && ucs <= 0x23ff);       // Misc Technical (emoji subset)

    return 1 +
        (wide_emoji ||
         (ucs >= 0x1100 &&
          (ucs <= 0x115f ||                       // Hangul Jamo init. consonants
           ucs == 0x2329 || ucs == 0x232a ||
           (ucs >= 0x2e80 && ucs <= 0xa4cf &&
            ucs != 0x303f) ||                     // CJK ... Yi
           (ucs >= 0xac00 && ucs <= 0xd7a3) ||    // Hangul Syllables
           (ucs >= 0xf900 && ucs <= 0xfaff) ||    // CJK Compatibility Ideographs
           (ucs >= 0xfe10 && ucs <= 0xfe19) ||    // Vertical forms
           (ucs >= 0xfe30 && ucs <= 0xfe6f) ||    // CJK Compatibility Forms
           (ucs >= 0xff00 && ucs <= 0xff60) ||    // Fullwidth Forms
           (ucs >= 0xffe0 && ucs <= 0xffe6) ||
           (ucs >= 0x20000 && ucs <= 0x2fffd) ||
           (ucs >= 0x30000 && ucs <= 0x3fffd))));
}

// Decode one UTF-8 codepoint starting at text[pos].
// Returns {codepoint, bytes_consumed}. Invalid bytes consume 1 and return U+FFFD.
std::pair<char32_t, std::size_t> decode_utf8(std::string_view text, std::size_t pos) {
    if (pos >= text.size()) return {U'\0', 0};
    unsigned char c0 = static_cast<unsigned char>(text[pos]);
    if (c0 < 0x80) return {static_cast<char32_t>(c0), 1};

    auto valid_cont = [&](std::size_t p) -> bool {
        return p < text.size() && (static_cast<unsigned char>(text[p]) & 0xC0) == 0x80;
    };

    if ((c0 & 0xE0) == 0xC0) {
        if (!valid_cont(pos + 1)) return {U'\uFFFD', 1};
        char32_t cp = ((c0 & 0x1F) << 6) |
                      (static_cast<unsigned char>(text[pos + 1]) & 0x3F);
        if (cp < 0x80) return {U'\uFFFD', 1}; // overlong
        return {cp, 2};
    }
    if ((c0 & 0xF0) == 0xE0) {
        if (!valid_cont(pos + 1) || !valid_cont(pos + 2)) return {U'\uFFFD', 1};
        char32_t cp = ((c0 & 0x0F) << 12) |
                      ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6) |
                      (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
        if (cp < 0x800) return {U'\uFFFD', 1}; // overlong
        return {cp, 3};
    }
    if ((c0 & 0xF8) == 0xF0) {
        if (!valid_cont(pos + 1) || !valid_cont(pos + 2) || !valid_cont(pos + 3)) {
            return {U'\uFFFD', 1};
        }
        char32_t cp = ((c0 & 0x07) << 18) |
                      ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 12) |
                      ((static_cast<unsigned char>(text[pos + 2]) & 0x3F) << 6) |
                      (static_cast<unsigned char>(text[pos + 3]) & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) return {U'\uFFFD', 1};
        return {cp, 4};
    }
    return {U'\uFFFD', 1};
}

// Skip an ANSI escape sequence starting at text[pos] (text[pos] == '\x1b').
// Returns the number of bytes that belong to the sequence.
std::size_t skip_ansi(std::string_view text, std::size_t pos) {
    if (pos >= text.size() || text[pos] != '\x1b') return 0;
    if (pos + 1 >= text.size()) return 1;
    unsigned char second = static_cast<unsigned char>(text[pos + 1]);

    // CSI: ESC [ ... final_byte
    if (second == '[') {
        std::size_t i = pos + 2;
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0x40 && c <= 0x7E) return i - pos + 1;
            ++i;
        }
        return text.size() - pos; // unterminated
    }

    // OSC: ESC ] ... BEL or ESC 0x1B 0x5C
    if (second == ']') {
        std::size_t i = pos + 2;
        while (i < text.size()) {
            if (text[i] == '\x07') return i - pos + 1;
            if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '\\') {
                return i - pos + 2;
            }
            ++i;
        }
        return text.size() - pos;
    }

    // Two-character escapes: ESC ( ) # c 7 8 M etc.
    return 2;
}

} // namespace

std::size_t visible_width(std::string_view text) {
    std::size_t width = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\x1b') {
            i += skip_ansi(text, i);
            continue;
        }
        if (c == '\t') {
            width += 8 - (width % 8);
            ++i;
            continue;
        }
        auto [cp, n] = decode_utf8(text, i);
        i += n;
        int w = codepoint_display_width(cp);
        if (w > 0) width += static_cast<std::size_t>(w);
    }
    return width;
}

std::string strip_ansi(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\x1b') {
            std::size_t skip = skip_ansi(text, i);
            i += skip;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::vector<std::string> wrap_lines(std::string_view text, std::size_t width) {
    std::vector<std::string> lines;
    if (width == 0) {
        lines.emplace_back(text);
        return lines;
    }

    std::string current;
    std::size_t current_width = 0;

    auto flush = [&]() {
        if (!current.empty()) {
            lines.push_back(std::move(current));
            current.clear();
            current_width = 0;
        }
    };

    auto append_word = [&](std::string_view word) {
        std::size_t w = visible_width(word);
        if (w == 0) return;
        if (!current.empty() && current_width + 1 + w > width) {
            flush();
        }
        if (!current.empty()) {
            current += ' ';
            ++current_width;
        }
        current.append(word);
        current_width += w;
    };

    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || std::isspace(static_cast<unsigned char>(text[i]))) {
            if (i > start) {
                append_word(text.substr(start, i - start));
            }
            if (i < text.size() && text[i] == '\n') {
                flush();
            }
            start = i + 1;
        }
    }
    flush();
    return lines;
}

std::string truncate(std::string_view text, std::size_t width, std::string_view ellipsis) {
    if (visible_width(text) <= width) return std::string(text);

    std::size_t ew = visible_width(ellipsis);
    if (ew >= width) {
        // No room even for the ellipsis: keep only whole codepoints that
        // fit (a raw byte substr could split a UTF-8 sequence).
        std::string out;
        std::size_t w = 0;
        std::size_t i = 0;
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (c == '\x1b') { i += skip_ansi(text, i); continue; }
            auto [cp, n] = decode_utf8(text, i);
            const int cw = codepoint_display_width(cp);
            if (cw > 0 && w + static_cast<std::size_t>(cw) > width) break;
            out.append(text.substr(i, n));
            w += static_cast<std::size_t>(cw > 0 ? cw : 0);
            i += n;
        }
        return out;
    }

    std::string out;
    out.reserve(text.size());
    std::size_t w = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '\x1b') {
            std::size_t skip = skip_ansi(text, i);
            out.append(text.substr(i, skip));
            i += skip;
            continue;
        }
        if (c == '\t') {
            std::size_t tab_width = 8 - (w % 8);
            if (w + tab_width + ew > width) {
                out.append(ellipsis);
                break;
            }
            out.push_back('\t');
            w += tab_width;
            ++i;
            continue;
        }
        auto [cp, n] = decode_utf8(text, i);
        int cw = codepoint_display_width(cp);
        if (cw < 0) cw = 0;
        if (w + static_cast<std::size_t>(cw) + ew > width && cw > 0) {
            out.append(ellipsis);
            break;
        }
        out.append(text.substr(i, n));
        w += static_cast<std::size_t>(cw);
        i += n;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Drawing primitives.
// ---------------------------------------------------------------------------
std::string hrule(std::size_t width, char ascii, const RenderConfig& cfg) {
    if (cfg.unicode_enabled) {
        std::string line = "─";
        line.reserve(width);
        for (std::size_t i = 1; i < width; ++i) line += "─";
        return line;
    }
    return std::string(width, ascii);
}

BoxResult make_box(const std::vector<std::string>& content,
                   std::optional<std::string_view> title,
                   const RenderConfig& cfg,
                   Alignment align) {
    BoxResult result;
    std::size_t max_inner = std::accumulate(
        content.begin(), content.end(), std::size_t{0},
        [](std::size_t m, const std::string& line) { return std::max(m, visible_width(line)); });
    if (title) {
        max_inner = std::max(max_inner, visible_width(*title) + 2); // space around title
    }
    if (max_inner == 0) max_inner = 1;

    // Clamp to the live terminal width if known (resize-aware).
    const int live_width = effective_terminal_width(cfg);
    if (live_width > 0) {
        std::size_t available = live_width > 4 ? static_cast<std::size_t>(live_width) - 4 : 1;
        max_inner = std::min(max_inner, available);
    }
    result.inner_width = max_inner;

    auto h = box_char(u'─', cfg.unicode_enabled);
    auto v = box_char(u'│', cfg.unicode_enabled);
    auto tl = box_char(u'╭', cfg.unicode_enabled);
    auto tr = box_char(u'╮', cfg.unicode_enabled);
    auto bl = box_char(u'╰', cfg.unicode_enabled);
    auto br = box_char(u'╯', cfg.unicode_enabled);

    if (title) {
        std::string styled_title = cfg.color_enabled
            ? std::string("\x1b[1m") + std::string(*title) + std::string(kReset)
            : std::string(*title);
        std::size_t title_w = visible_width(styled_title);
        std::size_t total = max_inner + 2;
        std::size_t dashes = total > title_w + 2 ? total - title_w - 2 : 0;
        std::size_t left_dashes = dashes / 2;
        std::size_t right_dashes = dashes - left_dashes;

        std::string top = tl;
        for (std::size_t i = 0; i < left_dashes; ++i) top += h;
        top += " " + styled_title + " ";
        for (std::size_t i = 0; i < right_dashes; ++i) top += h;
        top += tr;
        result.lines.push_back(top);
    } else {
        std::string top = tl;
        for (std::size_t i = 0; i < max_inner + 2; ++i) top += h;
        top += tr;
        result.lines.push_back(top);
    }

    for (const auto& line : content) {
        std::string out = v + " ";
        std::string truncated = truncate(line, max_inner, "…");
        out += truncated;
        std::size_t pad = max_inner - visible_width(truncated);
        if (pad > 0) out.append(pad, ' ');
        out += " " + v;
        result.lines.push_back(out);
    }

    std::string bottom = bl;
    for (std::size_t i = 0; i < max_inner + 2; ++i) bottom += h;
    bottom += br;
    result.lines.push_back(bottom);

    // Apply horizontal alignment relative to the terminal width.
    // Auto always resolves to Left: agent output is always left-justified.
    if (live_width > 0) {
        std::size_t tw = static_cast<std::size_t>(live_width);
        if (align == Alignment::Auto) {
            align = Alignment::Left;
        }
        if (align == Alignment::Right || align == Alignment::Center) {
            for (auto& line : result.lines) {
                std::size_t vw = visible_width(line);
                if (vw < tw) {
                    std::size_t pad = tw - vw;
                    if (align == Alignment::Center) pad /= 2;
                    line.insert(line.begin(), pad, ' ');
                }
            }
        }
    }

    return result;
}

std::vector<std::string> make_table(const std::vector<TableColumn>& columns,
                                    const std::vector<std::vector<std::string>>& rows,
                                    const RenderConfig& cfg) {
    std::vector<std::string> out;
    if (columns.empty()) return out;

    std::vector<std::size_t> widths(columns.size());
    for (std::size_t c = 0; c < columns.size(); ++c) {
        widths[c] = columns[c].min_width;
        widths[c] = std::max(widths[c], visible_width(columns[c].header));
        for (const auto& row : rows) {
            if (c < row.size()) {
                widths[c] = std::max(widths[c], visible_width(row[c]));
            }
        }
        if (columns[c].max_width > 0) {
            widths[c] = std::min(widths[c], columns[c].max_width);
        }
    }

    // Fit the table to the live terminal width (resize-aware). Two passes:
    // first SHRINK over-wide columns when the natural widths overflow the
    // terminal, then GROW flex columns when there is a surplus.
    const int live_width = effective_terminal_width(cfg);
    if (live_width > 0) {
        // Total visible width of a border or row line:
        // 1 (left border) + Σ(width + 3) per column (2 padding + border).
        std::size_t used = 1 + std::accumulate(
            widths.begin(), widths.end(), std::size_t{0},
            [](std::size_t u, std::size_t w) { return u + w + 3; });
        std::size_t tw = static_cast<std::size_t>(live_width);

        // Shrink pass: table is wider than the terminal. Shave the
        // currently widest shrinkable column one char at a time so the
        // reduction lands mostly on the widest (usually flex) columns.
        // Never go below a column's min_width. Cells longer than the
        // shrunk width are truncated with "…" by the cell() lambda.
        for (std::size_t excess = used > tw ? used - tw : 0; excess > 0;) {
            std::size_t best = columns.size();
            std::size_t best_slack = 0;
            for (std::size_t c = 0; c < columns.size(); ++c) {
                std::size_t slack = widths[c] > columns[c].min_width
                    ? widths[c] - columns[c].min_width : 0;
                if (slack > best_slack) {
                    best_slack = slack;
                    best = c;
                }
            }
            if (best == columns.size()) break; // everything at min_width
            --widths[best];
            --excess;
        }

        // Grow pass: distribute remaining width to flex columns.
        used = 1 + std::accumulate(
            widths.begin(), widths.end(), std::size_t{0},
            [](std::size_t u, std::size_t w) { return u + w + 3; });
        std::size_t remaining = tw > used ? tw - used : 0;
        for (std::size_t c = 0; c < columns.size() && remaining > 0; ++c) {
            if (columns[c].flex) {
                std::size_t add = remaining / (columns.size() - c);
                widths[c] += add;
                remaining -= add;
            }
        }
    }

    auto h = box_char(u'─', cfg.unicode_enabled);
    auto v = box_char(u'│', cfg.unicode_enabled);
    auto tl = box_char(u'╭', cfg.unicode_enabled);
    auto tm = box_char(u'┬', cfg.unicode_enabled);
    auto tr = box_char(u'╮', cfg.unicode_enabled);
    auto ml = box_char(u'├', cfg.unicode_enabled);
    auto mm = box_char(u'┼', cfg.unicode_enabled);
    auto mr = box_char(u'┤', cfg.unicode_enabled);
    auto bl = box_char(u'╰', cfg.unicode_enabled);
    auto bm = box_char(u'┴', cfg.unicode_enabled);
    auto br = box_char(u'╯', cfg.unicode_enabled);

    auto border = [&](const std::string& left,
                      const std::string& mid,
                      const std::string& right) -> std::string {
        std::string out_line = left;
        for (std::size_t c = 0; c < columns.size(); ++c) {
            for (std::size_t i = 0; i < widths[c] + 2; ++i) out_line += h;
            out_line += (c + 1 < columns.size()) ? mid : right;
        }
        return out_line;
    };

    auto cell = [&](std::string_view text, std::size_t width) -> std::string {
        std::string t = truncate(text, width, "…");
        std::size_t pad = width - visible_width(t);
        std::string res = " ";
        res += t;
        res.append(pad, ' ');
        res += " ";
        return res;
    };

    std::string header = v;
    for (std::size_t c = 0; c < columns.size(); ++c) {
        std::string htxt = cfg.color_enabled
            ? std::string("\x1b[1m") + std::string(columns[c].header) + std::string(kReset)
            : std::string(columns[c].header);
        header += cell(htxt, widths[c]) + v;
    }

    out.push_back(border(tl, tm, tr));
    out.push_back(header);
    out.push_back(border(ml, mm, mr));

    for (const auto& row : rows) {
        std::string line = v;
        for (std::size_t c = 0; c < columns.size(); ++c) {
            std::string_view text = c < row.size() ? std::string_view(row[c]) : std::string_view();
            line += cell(text, widths[c]) + v;
        }
        out.push_back(line);
    }
    out.push_back(border(bl, bm, br));
    return out;
}

// ---------------------------------------------------------------------------
// Progress.
// ---------------------------------------------------------------------------
std::string progress_bar(double fraction, std::size_t width, const RenderConfig& cfg) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    std::size_t filled = static_cast<std::size_t>(std::round(fraction * static_cast<double>(width)));
    std::string bar;
    bar.reserve(width * 3);
    for (std::size_t i = 0; i < width; ++i) {
        bar += i < filled ? "█" : "░";
    }
    if (!cfg.unicode_enabled) {
        std::string ascii;
        ascii.reserve(width);
        for (std::size_t i = 0; i < width; ++i) ascii += i < filled ? '#' : '-';
        return ascii;
    }
    return bar;
}

std::string compact_progress(std::string_view label,
                             double fraction,
                             std::size_t bar_width,
                             const RenderConfig& cfg) {
    std::ostringstream oss;
    int pct = static_cast<int>(std::round(fraction * 100.0));
    oss << label << " " << progress_bar(fraction, bar_width, cfg)
        << " " << pct << "%";
    if (cfg.color_enabled) {
        Style s(cfg);
        return s.info(oss.str());
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Code highlighting.
// ---------------------------------------------------------------------------
std::optional<std::string_view> detect_fence(std::string_view line) {
    if (line.size() >= 3 && line.substr(0, 3) == "```") {
        std::string_view rest = line.substr(3);
        // Trim trailing whitespace.
        std::size_t end = rest.find_first_of(" \t\r\n");
        if (end == std::string_view::npos) end = rest.size();
        return rest.substr(0, end);
    }
    return std::nullopt;
}

std::string highlight_code_line(std::string_view line,
                                std::string_view language,
                                const RenderConfig& cfg) {
    if (!cfg.color_enabled) return std::string(line);

    const auto& keywords = keywords_for(language);
    std::string out;
    out.reserve(line.size() * 2);

    std::size_t i = 0;
    while (i < line.size()) {
        // Single-line comment.
        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/') {
            out += "\x1b[2m";
            out += line.substr(i);
            out += kReset;
            break;
        }
        if (line[i] == '#') {
            out += "\x1b[2m";
            out += line.substr(i);
            out += kReset;
            break;
        }
        // String literal (single or double quote).
        if (line[i] == '"' || line[i] == '\'') {
            char quote = line[i];
            std::size_t j = i + 1;
            while (j < line.size() && line[j] != quote) {
                if (line[j] == '\\' && j + 1 < line.size()) j += 2;
                else ++j;
            }
            if (j < line.size()) ++j;
            out += "\x1b[32m";
            out += line.substr(i, j - i);
            out += kReset;
            i = j;
            continue;
        }
        // Number.
        if (std::isdigit(static_cast<unsigned char>(line[i]))) {
            std::size_t j = i;
            while (j < line.size() &&
                   (std::isdigit(static_cast<unsigned char>(line[j])) ||
                    line[j] == '.' || line[j] == 'x' || line[j] == 'X' ||
                    (line[j] >= 'a' && line[j] <= 'f') ||
                    (line[j] >= 'A' && line[j] <= 'F'))) {
                ++j;
            }
            out += "\x1b[38;5;208m";
            out += line.substr(i, j - i);
            out += kReset;
            i = j;
            continue;
        }
        // Identifier / keyword.
        if (std::isalpha(static_cast<unsigned char>(line[i])) || line[i] == '_') {
            std::size_t j = i;
            while (j < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) {
                ++j;
            }
            std::string_view word = line.substr(i, j - i);
            if (keywords.count(word)) {
                out += "\x1b[38;5;39m\x1b[1m";
                out += word;
                out += kReset;
            } else {
                out += word;
            }
            i = j;
            continue;
        }
        out.push_back(line[i]);
        ++i;
    }
    return out;
}

} // namespace icarus::ui
