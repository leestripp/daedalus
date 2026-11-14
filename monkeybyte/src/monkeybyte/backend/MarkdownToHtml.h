#pragma once

#include <string>

namespace monkeybyte {

// §6 preview tabs — true markdown → HTML conversion for the editor-area
// markdown viewer (WebKit-backed, see ui/HtmlView). GitHub-flavored markdown
// via md4c (tables, strikethrough, task lists). Kept GTK-free so it is
// unit-testable; separate from the chat's lightweight Pango renderer (§7.4).
class MarkdownToHtml {
public:
    // Markdown body fragment (no <html>/<body> wrapper). Raw HTML in the
    // source is dropped (md4c MD_FLAG_NOHTML) so scripts cannot execute.
    static std::string to_body(const std::string& md);

    // Full standalone HTML document with embedded Catppuccin-style CSS
    // (dark = Mocha, light = Latte), ready for webkit_web_view_load_html.
    static std::string to_document(const std::string& md, bool dark);
};

} // namespace monkeybyte
