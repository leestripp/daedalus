#include "tools/SearxngSearchTool.h"

#include "core/SearxngConfig.h"
#include "core/FileIO.h"

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>
#include <utils/Utils.h>

#include <chrono>
#include <string>

// Helper function to get the SearXNG config from settings.
// Delegates to the shared SearxngConfig helper, which is the single source
// of truth for the lookup (SettingsStore searxng_config table).
//
// IMPORTANT: do NOT reimplement the lookup logic here - the helper
// is also called by the unit tests, so any change to the lookup
// algorithm must happen in SearxngConfig.cpp.
static daedalus::SearxngConfigResult get_searxng_config() {
    return daedalus::load_searxng_config();
}

json SearxngSearchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Perform a web search using the configured self-hosted SearXNG metasearch instance. Returns search results with titles, URLs, and snippets. Requires the SearXNG instance to be configured in settings (searxng_config)."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"query", json::object({
                        {"type", "string"},
                        {"description", "The search query to perform"}
                    })},
                    {"max_results", json::object({
                        {"type", "integer"},
                        {"description", "Maximum number of results to return (default 10, capped at 25)"}
                    })}
                })},
                {"required", json::array({"query"})}
            })}
        }}
    });
}

json SearxngSearchTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;  // Not used for SearXNG search

    std::string query;
    int max_results = 10;

    try {
        if (args.is_object()) {
            query = args.value("query", "");
            max_results = args.value("max_results", 10);
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            query = parsed_args.value("query", "");
            max_results = parsed_args.value("max_results", 10);
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for searxng_search"}});
        }

        if (query.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'query' argument"}});
        }
        if (max_results < 1) max_results = 1;
        if (max_results > 25) max_results = 25;
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    // Get instance config
    const auto cfg_result = get_searxng_config();
    if (!cfg_result.ok()) {
        return json::object({
            {"status", "error"},
            {"message", std::string("SearXNG not configured (") +
                        cfg_result.status_name() + "): " +
                        cfg_result.error_detail}
        });
    }
    const auto& cfg = cfg_result.settings;

    try {
        std::string encoded_query = OmniNet::Utils::urlEncode(query);

        // GET {base_url}/search?q=...&format=json
        const std::string url = cfg.base_url + "/search?q=" + encoded_query
                              + "&format=json";

        // Build the request.
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method("GET");
        req.header("Accept", "application/json");
        req.header("User-Agent", "daedalus/1.0");

        // HTTP Basic auth when a username is configured (empty username =
        // unauthenticated instance, e.g. localhost-only SearXNG).
        if (!cfg.username.empty()) {
            const std::string creds = cfg.username + ":" + cfg.password;
            req.header("Authorization",
                       "Basic " + OmniNet::Utils::base64Encode(creds));
        }

        // 30s timeout — matches SerperSearchTool.
        OmniNet::HTTP::ClientConfig http_cfg;
        http_cfg.connect_timeout = std::chrono::seconds(10);
        http_cfg.request_timeout = std::chrono::seconds(30);
        http_cfg.follow_redirects = true;
        http_cfg.verify_tls       = true;

        // Perform the request (sync).
        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, http_cfg);

        if (!result.has_value()) {
            return json::object({
                {"status",  "error"},
                {"message", std::string("transport error: ") +
                                OmniNet::HTTP::clientErrorName(result.error())}});
        }

        const auto& resp = result.value();
        const int code = resp.statusCode();
        if (code == 401 || code == 403) {
            return json::object({
                {"status",  "error"},
                {"message", "SearXNG instance rejected the request (HTTP " +
                            std::to_string(code) +
                            ") — check username/password in searxng_config"}});
        }
        if (code != 200) {
            return json::object({
                {"status",  "error"},
                {"message", "SearXNG instance returned HTTP " + std::to_string(code)}});
        }

        // Parse the JSON response
        json api_response = json::parse(resp.bodyAsString());

        // Extract results from SearXNG format:
        // { "results": [ { "title", "url", "content", "engine", ... } ] }
        json results = json::array();

        if (api_response.contains("results") && api_response["results"].is_array()) {
            for (const auto& item : api_response["results"]) {
                if (static_cast<int>(results.size()) >= max_results) break;
                json result_item = json::object();
                result_item["title"]       = item.value("title", "");
                result_item["url"]         = item.value("url", "");
                result_item["description"] = item.value("content", "");
                if (item.contains("engine")) {
                    result_item["engine"] = item["engine"];
                }
                results.push_back(result_item);
            }
        }

        return json::object({
            {"status", "success"},
            {"query", query},
            {"count", results.size()},
            {"results", results}
        });

    } catch (const json::parse_error& e) {
        return json::object({{"status", "error"}, {"message", std::string("Failed to parse SearXNG response (is JSON format enabled on the instance?): ") + e.what()}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}
