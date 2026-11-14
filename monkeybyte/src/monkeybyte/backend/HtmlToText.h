#pragma once

#include <string>

namespace monkeybyte {

// Static HTML → plain text for agent consumption (design §6 web_fetch).
class HtmlToText {
public:
    // Drops <script>/<style>/<noscript> incl. their content, removes all
    // tags (block-level closers become newlines), decodes common HTML
    // entities, collapses runs of blank lines to one, trims the result.
    static std::string to_text(const std::string& html);
};

} // namespace monkeybyte
