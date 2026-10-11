#pragma once
// SPDX-License-Identifier: BSL-1.0
// src/tools/websearch/WebSearchProvider.h
//
// §WEB-SEARCH-CORE (2026-10-10): provider registry for the unified
// `web_search` tool. Replaces the one-tool-per-provider SerperSearchTool /
// SearxngSearchTool pair.
//
// Each provider normalises its API's response to WebSearchResult
// {title, url, snippet}. The failover chain (run_web_search_chain) walks
// enabled+configured providers in Settings priority order and returns the
// first non-empty result set — a blocked or erroring provider falls
// through to the next one instead of failing the search.
//
// The per-provider parse_* functions are PURE (string in, results out) so
// unit tests exercise response handling against canned fixtures without
// any network.

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "daedalus/settings.h"

namespace daedalus {

//! Normalised search result shared by every provider.
struct WebSearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

using WebSearchResults = std::vector<WebSearchResult>;

// ---------------------------------------------------------------------------
// Pure response parsers (unit-testable without network)
// ---------------------------------------------------------------------------

//! Minimal HTML -> text: drops tags, decodes common HTML entities,
//! collapses whitespace. Used by the DuckDuckGo HTML provider.
std::string web_search_html_to_text(std::string_view html);

//! DuckDuckGo html.duckduckgo.com results page parser. Extracts result__a
//! anchors (decoding /l/?uddg= redirect URLs) and result__snippet elements.
WebSearchResults parse_duckduckgo_html(std::string_view html,
                                       std::size_t max_results);

//! Bing HTML results page parser (key-less). Extracts b_algo blocks,
//! decoding /ck/a redirect URLs (u= param, "a1" + base64url).
//! §WEB-SEARCH-HYBRID (2026-10-11).
WebSearchResults parse_bing_html(std::string_view html,
                                 std::size_t max_results);

//! Brave Search API (api.search.brave.com/res/v1/web/search) JSON body.
WebSearchResults parse_brave_response(std::string_view body,
                                      std::size_t max_results);

//! Tavily /search JSON body.
WebSearchResults parse_tavily_response(std::string_view body,
                                       std::size_t max_results);

//! Exa /search JSON body (contents.text requested).
WebSearchResults parse_exa_response(std::string_view body,
                                    std::size_t max_results);

//! Firecrawl /v2/search JSON body.
WebSearchResults parse_firecrawl_response(std::string_view body,
                                          std::size_t max_results);

//! Serper.dev google.serper.dev/search JSON body.
WebSearchResults parse_serper_response(std::string_view body,
                                       std::size_t max_results);

//! SerpAPI serpapi.com/search.json JSON body.
WebSearchResults parse_serpapi_response(std::string_view body,
                                        std::size_t max_results);

//! SearXNG {base}/search?format=json JSON body.
WebSearchResults parse_searxng_response(std::string_view body,
                                        std::size_t max_results);

// ---------------------------------------------------------------------------
// Provider interface
// ---------------------------------------------------------------------------

//! One web-search backend. Implementations live in WebSearchProvider.cpp.
class WebSearchProvider {
public:
    virtual ~WebSearchProvider() = default;

    //! Stable provider id (matches WebSearchProviderSettings::name).
    virtual std::string name() const = 0;

    //! Human-readable name for errors / settings UIs.
    virtual std::string display_name() const = 0;

    //! True when `cfg` has everything this provider needs (API key where
    //! required, base_url for searxng). Unconfigured providers are skipped
    //! silently by the failover chain.
    virtual bool is_configured(
        const WebSearchProviderSettings& cfg) const = 0;

    //! Execute one search. Errors are returned, never thrown (transport,
    //! HTTP status, and JSON problems all become the error string).
    virtual std::expected<WebSearchResults, std::string>
    search(const std::string& query, std::size_t max_results,
           const WebSearchProviderSettings& cfg) const = 0;
};

//! The 8 built-in provider ids, in default priority order.
const std::vector<std::string>& web_search_provider_ids();

//! Construct a provider by id, or nullptr for an unknown id.
std::unique_ptr<WebSearchProvider>
make_web_search_provider(std::string_view id);

// ---------------------------------------------------------------------------
// Failover chain (§WEB-SEARCH-FAILOVER)
// ---------------------------------------------------------------------------

struct WebSearchFailure {
    std::string provider;  //!< provider id
    std::string error;     //!< transport / HTTP / parse / empty-results
};

struct WebSearchOutcome {
    bool ok = false;
    std::string provider;    //!< provider that produced the results
    WebSearchResults results;
    std::vector<WebSearchFailure> failures;  //!< attempts that failed, in order
};

//! Walk `chain` (enabled providers, priority order) until one returns a
//! non-empty result set. Unknown ids, unconfigured rows, and provider
//! errors all fall through; every failure is recorded in the outcome.
//!
//! `make` is the provider factory; pass a fake in tests. nullptr selects
//! make_web_search_provider.
WebSearchOutcome run_web_search_chain(
    const std::vector<WebSearchProviderSettings>& chain,
    const std::string& query, std::size_t max_results,
    std::unique_ptr<WebSearchProvider> (*make)(std::string_view) = nullptr);

//! Load the enabled failover chain from the default SettingsStore
//! (Paths::global() user data root). Read at request time — Settings edits
//! take effect without a restart, same contract the legacy tools had.
std::vector<WebSearchProviderSettings> load_web_search_chain();

//! One-line startup status for hosts to log, e.g.
//! "[WEB-SEARCH] failover chain: serper -> searxng -> duckduckgo".
//! Returns a warning line when the chain is empty.
std::string web_search_startup_status();

}  // namespace daedalus
