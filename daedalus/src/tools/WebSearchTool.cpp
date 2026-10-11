// SPDX-License-Identifier: BSL-1.0
// src/tools/WebSearchTool.cpp

#include "tools/WebSearchTool.h"

#include "core/FileIO.h"
#include "tools/websearch/WebSearchProvider.h"

#include <algorithm>
#include <string>

json WebSearchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description",
             "Search the web and return result titles, URLs, and snippets. "
             "NOTE: if you have a built-in web browser tool, you can also "
             "search like a human — open a search engine page (Google, "
             "Bing, DuckDuckGo) and type the query. web_fetch on a search "
             "engine URL also works (may be rate-limited by some engines). "
             "This tool uses the providers enabled in Settings, in "
             "priority order, with automatic failover when a provider "
             "errors or is blocked."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"query", json::object({
                        {"type", "string"},
                        {"description", "The search query to perform"}
                    })},
                    {"max_results", json::object({
                        {"type", "integer"},
                        {"description", "Maximum number of results to return (default 8, capped at 25)"}
                    })},
                    {"provider", json::object({
                        {"type", "string"},
                        {"description",
                         "Optional: force one provider instead of the "
                         "failover chain. One of: brave, tavily, exa, "
                         "firecrawl, serper, serpapi, searxng, duckduckgo."}
                    })}
                })},
                {"required", json::array({"query"})}
            })}
        }}
    });
}

json WebSearchTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;  // not used — search is network-only

    std::string query;
    std::string forced_provider;
    int max_results = 8;

    try {
        json a;
        if (args.is_object()) {
            a = args;
        } else if (args.is_string()) {
            a = json::parse(args.get<std::string>());
        } else {
            return json::object({{"status", "error"},
                                 {"message", "Invalid arguments format for web_search"}});
        }
        query = a.value("query", "");
        forced_provider = a.value("provider", "");
        max_results = a.value("max_results", 8);

        if (query.empty()) {
            return json::object({{"status", "error"},
                                 {"message", "Missing or empty 'query' argument"}});
        }
        max_results = std::clamp(max_results, 1, 25);
    } catch (const std::exception& e) {
        return json::object({{"status", "error"},
                             {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    // Config per call — Settings edits apply without a restart.
    auto chain = daedalus::load_web_search_chain();

    // Optional forced provider: build a one-entry chain. Forcing works
    // even when the provider is disabled in Settings (explicit user
    // intent beats the toggle), but not when it is unconfigured.
    if (!forced_provider.empty()) {
        const auto it = std::find_if(
            chain.begin(), chain.end(),
            [&](const auto& p) { return p.name == forced_provider; });
        if (it == chain.end()) {
            return json::object({
                {"status", "error"},
                {"message", "Unknown web_search provider '" + forced_provider +
                            "'. Known providers: brave, tavily, exa, "
                            "firecrawl, serper, serpapi, searxng, duckduckgo."}});
        }
        auto single = *it;
        single.enabled = true;
        chain = {std::move(single)};
    }

    const auto outcome = daedalus::run_web_search_chain(
        chain, query, static_cast<std::size_t>(max_results));

    if (!outcome.ok) {
        json failures = json::array();
        for (const auto& f : outcome.failures) {
            failures.push_back({{"provider", f.provider},
                                {"error", f.error}});
        }
        return json::object({
            {"status", "error"},
            {"message",
             failures.empty()
                 ? "web_search: no providers available — none of the "
                   "enabled providers in Settings are configured "
                   "(hybrid needs no key and works out of the box)"
                 : "web_search: every enabled provider failed — see "
                   "'failures' for per-provider reasons"},
            {"failures", failures}});
    }

    json results = json::array();
    for (const auto& r : outcome.results) {
        results.push_back({{"title", r.title},
                           {"url", r.url},
                           {"snippet", r.snippet}});
    }

    return json::object({
        {"status", "success"},
        {"provider", outcome.provider},
        {"query", query},
        {"count", results.size()},
        {"results", results}});
}
