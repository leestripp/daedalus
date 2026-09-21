#pragma once

#include "BaseTool.h"
#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * SearxngSearchTool - Web search via a self-hosted SearXNG metasearch
 * instance. Usage: searxng_search(query)
 *
 * §SEARXNG-CORE (2026-09-21): private, keyless search counterpart to
 * SerperSearchTool. Its configuration (SettingsStore searxng_config table,
 * core/SearxngConfig) lives in core, so the tool is registered by default;
 * without a configured base_url it returns a descriptive error instead of
 * executing (same contract as serper_search).
 */
class SearxngSearchTool : public BaseTool {
public:
    std::string get_name() const override { return "searxng_search"; }
    std::string get_description() const override { return "Perform web search using the configured self-hosted SearXNG metasearch instance"; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
