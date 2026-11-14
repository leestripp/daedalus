#include "monkeybyte/ui/chat/Markdown.h"

#include <cctype>
#include <unordered_set>

namespace monkeybyte {

namespace {

std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }
    }
    return out;
}

bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Process-wide active palette; initialized lazily so it can call
// Markdown::dark_palette() (main thread only — chat rendering is main-thread).
Markdown::Palette& active_palette() {
    static Markdown::Palette p = Markdown::dark_palette();
    return p;
}

} // namespace

Markdown::Palette Markdown::dark_palette() {
    // Catppuccin Mocha — matches data/theme/dark.css.
    return {"#6c7086", "#a6e3a1", "#fab387", "#cba6f7", "#313244"};
}

Markdown::Palette Markdown::light_palette() {
    // Catppuccin Latte — matches data/theme/light.css.
    return {"#6c6f85", "#40a02b", "#fe640b", "#8839ef", "#ccd0da"};
}

void Markdown::set_palette(const Palette& p) { active_palette() = p; }

const Markdown::Palette& Markdown::palette() { return active_palette(); }

std::string Highlighter::highlight(const std::string& code) {
    static const std::unordered_set<std::string> keywords = {
        "auto", "bool", "break", "case", "catch", "char", "class", "const",
        "continue", "def", "delete", "do", "double", "else", "enum", "false",
        "float", "for", "from", "func", "function", "if", "import", "in",
        "int", "let", "long", "new", "nullptr", "pass", "return", "short",
        "signed", "size_t", "static", "string", "struct", "switch", "template",
        "this", "throw", "true", "try", "type", "unsigned", "using", "var",
        "void", "while",
    };

    const auto& pal = Markdown::palette();
    std::string out;
    size_t i = 0;
    const size_t n = code.size();
    while (i < n) {
        const char c = code[i];
        // line comments
        if (c == '/' && i + 1 < n && code[i + 1] == '/') {
            size_t j = code.find('\n', i);
            if (j == std::string::npos) j = n;
            out += "<span foreground=\"" + pal.comment + "\">" + escape(code.substr(i, j - i)) + "</span>";
            i = j;
            continue;
        }
        if (c == '#') { // python/shell comment
            size_t j = code.find('\n', i);
            if (j == std::string::npos) j = n;
            out += "<span foreground=\"" + pal.comment + "\">" + escape(code.substr(i, j - i)) + "</span>";
            i = j;
            continue;
        }
        // strings
        if (c == '"' || c == '\'') {
            size_t j = i + 1;
            while (j < n && code[j] != c) {
                if (code[j] == '\\') ++j;
                ++j;
            }
            if (j < n) ++j;
            out += "<span foreground=\"" + pal.string + "\">" + escape(code.substr(i, j - i)) + "</span>";
            i = j;
            continue;
        }
        // numbers (incl. simple hex like 0x1F)
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t j = i;
            while (j < n) {
                const char d = code[j];
                if (std::isdigit(static_cast<unsigned char>(d)) || d == '.') {
                    ++j;
                } else if ((d == 'x' || d == 'X') && j == i + 1 && code[i] == '0') {
                    ++j;
                } else if (j > i + 1 && code[i] == '0' &&
                           (code[i + 1] == 'x' || code[i + 1] == 'X') &&
                           std::isxdigit(static_cast<unsigned char>(d))) {
                    ++j;
                } else {
                    break;
                }
            }
            out += "<span foreground=\"" + pal.number + "\">" + escape(code.substr(i, j - i)) + "</span>";
            i = j;
            continue;
        }
        // identifiers / keywords
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && is_word_char(code[j])) ++j;
            const std::string word = code.substr(i, j - i);
            if (keywords.count(word))
                out += "<span foreground=\"" + pal.keyword + "\">" + escape(word) + "</span>";
            else
                out += escape(word);
            i = j;
            continue;
        }
        out += escape(std::string(1, c));
        ++i;
    }
    return out;
}

std::string Markdown::to_pango(const std::string& md) {
    const auto& pal = palette();
    const std::string code_open =
        "<span font_family=\"monospace\" background=\"" + pal.code_bg + "\">";
    std::string out;
    size_t i = 0;
    const size_t n = md.size();
    bool first_block = true;

    auto newline = [&] {
        if (!first_block) out += "\n";
        first_block = false;
    };

    while (i < n) {
        // fenced code block ```lang ... ```
        if (md.compare(i, 3, "```") == 0) {
            size_t lang_end = md.find('\n', i + 3);
            if (lang_end == std::string::npos) lang_end = n;
            const size_t close = md.find("```", lang_end);
            const size_t body_end = (close == std::string::npos) ? n : close;
            // A fence with no newline yet (stream ends mid-```lang) has no
            // body — lang_end + 1 would run past the end of the string.
            const size_t body_start = lang_end < n ? lang_end + 1 : n;
            const std::string body = md.substr(body_start,
                body_end > body_start ? body_end - body_start : 0);
            newline();
            out += code_open + Highlighter::highlight(body) + "</span>";
            i = (close == std::string::npos) ? n : close + 3;
            continue;
        }

        // process one line at a time
        size_t line_end = md.find('\n', i);
        if (line_end == std::string::npos) line_end = n;
        std::string line = md.substr(i, line_end - i);
        i = line_end + 1;

        newline();

        // list item
        std::string prefix;
        if (line.size() > 2 && (line.compare(0, 2, "- ") == 0 ||
                                line.compare(0, 2, "* ") == 0)) {
            prefix = "• ";
            line = line.substr(2);
        }

        // inline formatting: `code`, **bold**, *italic*
        std::string rendered;
        for (size_t k = 0; k < line.size();) {
            if (line[k] == '`') {
                const size_t close = line.find('`', k + 1);
                if (close != std::string::npos) {
                    rendered += code_open +
                                escape(line.substr(k + 1, close - k - 1)) + "</span>";
                    k = close + 1;
                    continue;
                }
            }
            if (line.compare(k, 2, "**") == 0) {
                const size_t close = line.find("**", k + 2);
                if (close != std::string::npos) {
                    rendered += "<b>" + escape(line.substr(k + 2, close - k - 2)) + "</b>";
                    k = close + 2;
                    continue;
                }
            }
            if (line[k] == '*' && (k + 1 >= line.size() || line[k + 1] != '*')) {
                const size_t close = line.find('*', k + 1);
                if (close != std::string::npos && close > k + 1) {
                    rendered += "<i>" + escape(line.substr(k + 1, close - k - 1)) + "</i>";
                    k = close + 1;
                    continue;
                }
            }
            rendered += escape(std::string(1, line[k]));
            ++k;
        }
        out += escape(prefix) + rendered;
    }
    return out;
}

} // namespace monkeybyte
