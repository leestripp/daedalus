#pragma once
// SPDX-License-Identifier: BSL-1.0
// src/tools/WebSearchTool.h
//
// §WEB-SEARCH-CORE (2026-10-10): the single `web_search` tool. It walks
// the enabled web-search providers from Settings (web_search_providers
// table, schema v8) in priority order and returns the first non-empty
// result set — the §WEB-SEARCH-FAILOVER chain. Replaces the legacy
// one-tool-per-provider `serper_search` / `searxng_search` pair (removed;
// Serper and SearXNG survive as providers inside this tool).
//
// Config is read per call from the SettingsStore, so Settings edits take
// effect without a restart — same contract the legacy tools had.

#include "BaseTool.h"

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class WebSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "web_search"; }
    std::string get_description() const override {
        return "Search the web and return result titles, URLs, and snippets. "
               "NOTE: if you have a built-in web browser tool, you can also "
               "search like a human — open a search engine page (Google, "
               "Bing, DuckDuckGo) and type the query. web_fetch on a search "
               "engine URL also works (may be rate-limited by some engines). "
               "This tool uses the providers enabled in Settings, in "
               "priority order, with automatic failover when a provider "
               "errors or is blocked.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
