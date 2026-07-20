#include "SearchTool.h"
#include "core/SerperConfig.h"
#include "core/FileIO.h"
#include "api/HttpSession.h"

#include <OmniNet.h>
#include <http/HttpClient.h>
#include <http/HttpResponse.h>
#include <utils/Utils.h>

#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <stdexcept>
#include <sstream>
#include <cctype>
#include <cstring>

// ========== Helper Functions (from ToolHandler.cpp) ==========

namespace ToolHandler {
    bool is_cwd_initialized();
    std::filesystem::path get_current_working_directory();
}

// Escape shell argument
static std::string escape_shell_arg(const std::string& input) {
    std::string result;
    result.reserve(input.size() + 16);
    result.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            result.push_back('\'');
            result.push_back('\\');
            result.push_back('\'');
            result.push_back('\'');
        } else {
            result.push_back(c);
        }
    }
    result.push_back('\'');
    return result;
}

// ========== GrepSearchTool ==========

json GrepSearchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Search for a string or regex across the project. You can use common words like 'function', 'window', 'a', etc. Special regex characters like . * + ? are supported. Essential for finding function calls or class definitions in large C++ projects."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"query", json::object({
                        {"type", "string"},
                        {"description", "The search string or regex to find"}
                    })},
                    {"include_pattern", json::object({
                        {"type", "string"},
                        {"description", "Optional file pattern such as hpp files or cpp files"}
                    })}
                })},
                {"required", json::array({"query"})}
            })}
        }}
    });
}

json GrepSearchTool::execute(const json& args, FileIO& file_io) {
    std::string query;
    std::string include_pattern;
    try {
        if (args.is_object()) {
            query = args.value("query", "");
            include_pattern = args.value("include_pattern", "");
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            query = parsed_args.value("query", "");
            include_pattern = parsed_args.value("include_pattern", "");
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for grep_search"}});
        }

        if (query.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'query' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        std::filesystem::path base_path = ToolHandler::is_cwd_initialized() ?
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::string base_dir = base_path.string();

        // Validate query doesn't contain dangerous patterns
        std::vector<std::string> dangerous_patterns = {";", "&&", "||", "`", "\n", "\r", "\0"};
        for (const auto& pattern : dangerous_patterns) {
            if (pattern.empty()) continue;
            if (query.find(pattern) != std::string::npos) {
                return json::object({{"status", "error"}, {"message", "Query contains invalid characters"}});
            }
        }

        // Validate include_pattern
        if (!include_pattern.empty()) {
            for (char c : include_pattern) {
                if (!std::isalnum(c) && c != '.' && c != '*' && c != '_' && c != '-') {
                    return json::object({{"status", "error"}, {"message", "Include pattern contains invalid characters"}});
                }
            }
        }

        // Build grep command - use cd to change directory without affecting global state
        std::string command = "cd " + escape_shell_arg(base_dir) + " && grep -rnI --color=never -e " + escape_shell_arg(query);
        if (!include_pattern.empty()) {
            command += " --include=" + escape_shell_arg(include_pattern);
        }
        command += " .";

        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            return json::object({{"status", "error"}, {"message", "Failed to execute grep command"}});
        }

        const size_t max_output_size = 1024 * 1024;
        std::string output;
        output.reserve(65536);
        char buffer[256];
        size_t bytes_read = 0;
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            size_t len = strlen(buffer);
            if (bytes_read + len > max_output_size) {
                break;
            }
            output.append(buffer, len);
            bytes_read += len;
        }
        pclose(pipe);

        // Parse output
        json results = json::array();
        std::istringstream iss(output);
        std::string line;
        int result_count = 0;
        while (std::getline(iss, line) && result_count < 100) {
            if (!line.empty()) {
                if (line.find(base_dir) != std::string::npos || line.substr(0, 2) == "./") {
                    results.push_back(line);
                    result_count++;
                }
            }
        }

        return json::object({
            {"status", "success"},
            {"query", query},
            {"include_pattern", include_pattern},
            {"count", result_count},
            {"results", results},
            {"truncated", result_count >= 100}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== WikiSearchTool ==========

// Callback to handle the data received from Wikipedia API
json WikiSearchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Search Wikipedia for articles using the Wikimedia REST API. Returns a list of matching articles with titles and descriptions."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"query", json::object({
                        {"type", "string"},
                        {"description", "The search query/term to search Wikipedia for"}
                    })},
                    {"limit", json::object({
                        {"type", "integer"},
                        {"description", "Maximum number of results to return (default: 10, max: 20)"}
                    })}
                })},
                {"required", json::array({"query"})}
            })}
        }}
    });
}

json WikiSearchTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;  // Not used for Wikipedia search
    
    std::string query;
    int limit = 10;
    
    try {
        if (args.is_object()) {
            query = args.value("query", "");
            limit = args.value("limit", 10);
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            query = parsed_args.value("query", "");
            limit = parsed_args.value("limit", 10);
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for wiki_search"}});
        }

        if (query.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'query' argument"}});
        }

        // Validate and clamp limit
        if (limit < 1) limit = 1;
        if (limit > 20) limit = 20;
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        // URL-encode the query (manual encoder; the previous libcurl
        // path needed curl_easy_escape, but OmniNet::urlEncode also
        // exists — kept the local copy for now to minimise churn in
        // tests that exercise this exact encoding).
        auto url_encode = [](const std::string& s) -> std::string {
            std::string out;
            for (char c : s) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
                    c == '.' || c == '~') {
                    out += c;
                } else {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                    out += buf;
                }
            }
            return out;
        };

        // Build the URL using the Modern Wikimedia REST API
        std::string url = "https://api.wikimedia.org/core/v1/wikipedia/en/search/page?q="
                        + url_encode(query) + "&limit=" + std::to_string(limit);

        // §NC-4: Wikipedia search via OmniNet::HTTP::Client (was HttpSession).
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method("GET");
        req.header("User-Agent",
                   "daedalus/1.0 (https://github.com/daedalus; contact@example.com)");

        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(10);
        cfg.request_timeout = std::chrono::seconds(30);
        cfg.follow_redirects = true;

        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

        if (!result.has_value()) {
            return json::object({{"status", "error"},
                                 {"message", std::string("Wikipedia transport error: ") +
                                                OmniNet::HTTP::clientErrorName(result.error())}});
        }
        const auto& resp = result.value();
        if (resp.statusCode() != 200) {
            return json::object({
                {"status", "error"},
                {"message", "Wikipedia API returned HTTP " + std::to_string(resp.statusCode())}
            });
        }

        // Parse the JSON response
        json api_response = json::parse(resp.bodyAsString());

        // Extract results from the Wikimedia REST API format
        json results = json::array();

        if (api_response.contains("pages") && api_response["pages"].is_array()) {
            for (const auto& page : api_response["pages"]) {
                json result_item = json::object();
                result_item["title"] = page.value("title", "");
                result_item["description"] = page.value("description", "");

                // Extract excerpt if available
                if (page.contains("excerpt")) {
                    result_item["excerpt"] = page["excerpt"];
                }

                // Build the Wikipedia URL
                std::string title = page.value("title", "");
                if (!title.empty()) {
                    result_item["url"] = "https://en.wikipedia.org/wiki/" + url_encode(title);
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
        return json::object({{"status", "error"}, {"message", std::string("Failed to parse Wikipedia response: ") + e.what()}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== SerperSearchTool ==========

// Helper function to get the Serper API key from settings.
// Delegates to the shared SerperConfig helper, which is the single source
// of truth for the lookup. The helper accepts BOTH "serper-search"
// (canonical) and "serper" (legacy) for backward compatibility.
//
// IMPORTANT: do NOT reimplement the path-search logic here - the helper
// is also called by the startup check in main() and by the unit tests,
// so any change to the lookup algorithm must happen in SerperConfig.cpp.
static std::string get_serper_api_key() {
    return daedalus::load_serper_api_key().api_key;
}

json SerperSearchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", "Perform a web search using Google via the Serper.dev API. Returns search results with titles, URLs, and snippets. Requires Serper.dev API key in settings."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"query", json::object({
                        {"type", "string"},
                        {"description", "The search query to perform on Google"}
                    })}
                })},
                {"required", json::array({"query"})}
            })}
        }}
    });
}

json SerperSearchTool::execute(const json& args, FileIO& file_io) {
    (void)file_io;  // Not used for Serper search
    
    std::string query;
    
    try {
        if (args.is_object()) {
            query = args.value("query", "");
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            query = parsed_args.value("query", "");
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for serper_search"}});
        }

        if (query.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'query' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    // Get API key
    std::string api_key = get_serper_api_key();
    if (api_key.empty() || api_key == "YOUR_SERPER_API_KEY") {
        return json::object({
            {"status", "error"},
            {"message", "Serper.dev API key not configured. Add 'serper-search.api_key' "
                        "(or legacy 'serper.api_key') to ~/.daedalus/settings/settings.json"}
        });
    }

    try {
        // §NC-4 (2026-07-16): Serper search migrated from libcurl to
        // OmniNet::HTTP::Client. URL encoding via OmniNet::urlEncode();
        // POST via OmniNet::HTTP::Client::send_blocking() with explicit
        // Content-Type + X-API-Key + User-Agent headers.
        std::string encoded_query = OmniNet::Utils::urlEncode(query);

        // Build the URL (the `q=…` parameter is informational; the
        // POST body's `q` field is what Serper actually uses).
        const std::string url = "https://google.serper.dev/search?q=" + encoded_query;

        // Set up the POST body
        json post_body = json::object({
            {"q", query},
            {"num", 10}
        });
        const std::string post_body_str = post_body.dump();

        // Build the request.
        OmniNet::HTTP::ClientRequest req;
        req.url(url);
        req.method("POST");
        req.body(post_body_str);
        req.header("Content-Type", "application/json");
        req.header("X-API-Key",     api_key);
        req.header("User-Agent",    "daedalus/1.0");

        // 30s timeout — matches the old curl setting.
        OmniNet::HTTP::ClientConfig cfg;
        cfg.connect_timeout = std::chrono::seconds(10);
        cfg.request_timeout = std::chrono::seconds(30);
        cfg.follow_redirects = true;
        cfg.verify_tls       = true;

        // Perform the request (sync).
        auto result = OmniNet::HTTP::Client::get_instance().send_blocking(req, cfg);

        if (!result.has_value()) {
            return json::object({
                {"status",  "error"},
                {"message", std::string("transport error: ") +
                                OmniNet::HTTP::clientErrorName(result.error())}});
        }

        const auto& resp = result.value();
        const int code = resp.statusCode();
        if (code != 200) {
            return json::object({
                {"status",  "error"},
                {"message", "Serper API returned HTTP " + std::to_string(code)}});
        }

        // Parse the JSON response
        json api_response = json::parse(resp.bodyAsString());
        
        // Extract results from Serper API format
        json results = json::array();
        
        if (api_response.contains("organic") && api_response["organic"].is_array()) {
            for (const auto& item : api_response["organic"]) {
                json result_item = json::object();
                result_item["title"] = item.value("title", "");
                result_item["url"] = item.value("link", "");
                result_item["description"] = item.value("snippet", "");
                
                // Add position if available
                if (item.contains("position")) {
                    result_item["position"] = item["position"];
                }
                
                results.push_back(result_item);
            }
        }

        // Also include knowledge graph if available
        json extra = json::object();
        if (api_response.contains("knowledgeGraph")) {
            extra["knowledge_graph"] = api_response["knowledgeGraph"];
        }
        if (api_response.contains("answerBox") && !api_response["answerBox"].is_null()) {
            extra["answer_box"] = api_response["answerBox"];
        }

        json return_obj = json::object({
            {"status", "success"},
            {"query", query},
            {"count", results.size()},
            {"results", results}
        });
        
        if (!extra.empty()) {
            return_obj["extra"] = extra;
        }
        
        return return_obj;

    } catch (const json::parse_error& e) {
        return json::object({{"status", "error"}, {"message", std::string("Failed to parse Serper response: ") + e.what()}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}
