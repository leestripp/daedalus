#pragma once

#include <daedalus/types/BaseTool.h>

namespace monkeybyte {

// daedalus host tool: key-less web search for online research (design §6),
// via the DuckDuckGo HTML endpoint (backend/WebSearch). Follow up with
// web_fetch on a result URL.
class WebSearchTool : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "web_search"; }

    std::string get_description() const override {
        return "Search the web (DuckDuckGo, no API key) and return result "
               "titles, URLs, and snippets. Use for research, inspiration, "
               "and finding documentation; then call web_fetch on a result "
               "URL to read the page.";
    }

    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function",
             {{"name", get_name()},
              {"description", get_description()},
              {"parameters",
               {{"type", "object"},
                {"properties",
                 {{"query",
                   {{"type", "string"},
                    {"description", "search query"}}},
                  {"max_results",
                   {{"type", "integer"},
                    {"description",
                     "number of results (default 8, max 20)"}}}}},
                {"required", nlohmann::json::array({"query"})}}}}},
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext&) override;
};

} // namespace monkeybyte
