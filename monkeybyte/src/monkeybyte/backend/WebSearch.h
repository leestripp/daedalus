#pragma once

#include <expected>
#include <string>
#include <vector>

namespace monkeybyte {

// Key-less web search via the DuckDuckGo HTML endpoint (design §6
// web_search). The parser is separate from the network call so it can be
// unit-tested against canned HTML.
class WebSearch {
public:
    struct Result {
        std::string title;
        std::string url;
        std::string snippet;
    };

    // Pure parser for a DuckDuckGo html.duckduckgo.com results page — no
    // network. Extracts result__a anchors (href may be a /l/?uddg= redirect,
    // decoded to the real URL) and result__snippet elements. Tolerates
    // missing snippets.
    static std::vector<Result> parse(const std::string& html,
                                     std::size_t max_results);

    // GET https://html.duckduckgo.com/html/?q=<urlencoded> via WebFetch and
    // parse it. Returns up to max_results entries.
    static std::expected<std::vector<Result>, std::string>
    search(const std::string& query, std::size_t max_results = 8);
};

} // namespace monkeybyte
