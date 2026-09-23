#include "monkeybyte/backend/MarkdownToHtml.h"

#include <md4c-html.h>

namespace monkeybyte {

std::string MarkdownToHtml::to_body(const std::string& md) {
    // GitHub-flavored flags: tables, strikethrough, task lists, autolinks.
    // MD_FLAG_NOHTML drops raw HTML (scripts etc.) instead of emitting it.
    constexpr unsigned kFlags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH |
                                MD_FLAG_TASKLISTS | MD_FLAG_PERMISSIVEAUTOLINKS |
                                MD_FLAG_NOHTML;
    std::string out;
    md_html(md.data(), static_cast<MD_SIZE>(md.size()),
            [](const MD_CHAR* chunk, MD_SIZE size, void* userdata) {
                static_cast<std::string*>(userdata)->append(chunk, size);
            },
            &out, kFlags, /*renderer_flags=*/0);
    return out;
}

namespace {

// Catppuccin Mocha / Latte, matching data/theme/*.css (§11).
const char* kCssTemplate = R"css(
body {
  font-family: sans-serif;
  font-size: 15px;
  line-height: 1.6;
  max-width: 900px;
  margin: 0 auto;
  padding: 24px 32px;
  background: %BG%;
  color: %FG%;
}
h1, h2 { border-bottom: 1px solid %SURFACE%; padding-bottom: 6px; }
h1 { font-size: 1.8em; } h2 { font-size: 1.45em; } h3 { font-size: 1.2em; }
a { color: %ACCENT%; text-decoration: none; }
a:hover { text-decoration: underline; }
code {
  font-family: monospace;
  background: %SURFACE%;
  border-radius: 4px;
  padding: 2px 5px;
  font-size: 0.92em;
}
pre {
  background: %CODEBG%;
  border-radius: 6px;
  padding: 12px 16px;
  overflow-x: auto;
}
pre code { background: none; padding: 0; }
blockquote {
  margin: 0;
  padding: 2px 16px;
  border-left: 3px solid %ACCENT%;
  color: %MUTED%;
  background: %CODEBG%;
}
table { border-collapse: collapse; }
th, td { border: 1px solid %SURFACE%; padding: 6px 12px; }
th { background: %CODEBG%; }
img { max-width: 100%; }
hr { border: none; border-top: 1px solid %SURFACE%; }
)css";

std::string with_palette(const char* css, bool dark) {
    struct { const char* token; const char* dark; const char* light; } const colors[] = {
        {"%BG%",      "#1e1e2e", "#eff1f5"},
        {"%FG%",      "#cdd6f4", "#4c4f69"},
        {"%ACCENT%",  "#89b4fa", "#1e66f5"},
        {"%SURFACE%", "#45475a", "#ccd0da"},
        {"%CODEBG%",  "#181825", "#e6e9ef"},
        {"%MUTED%",   "#a6adc8", "#6c6f85"},
    };
    std::string out(css);
    for (const auto& c : colors) {
        const std::string token(c.token);
        const std::string value(dark ? c.dark : c.light);
        std::string::size_type pos = 0;
        while ((pos = out.find(token, pos)) != std::string::npos) {
            out.replace(pos, token.size(), value);
            pos += value.size();
        }
    }
    return out;
}

} // namespace

std::string MarkdownToHtml::to_document(const std::string& md, bool dark) {
    std::string doc = "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
                      "<style>";
    doc += with_palette(kCssTemplate, dark);
    doc += "</style>\n</head>\n<body>\n";
    doc += to_body(md);
    doc += "\n</body>\n</html>\n";
    return doc;
}

} // namespace monkeybyte
