#pragma once

#include <daedalus/types/BaseTool.h>

namespace monkeybyte {

// daedalus host tool: tab-less HTTP fetch for online research (design §6).
// HTML pages are converted to plain text (backend/HtmlToText); everything
// else (markdown, JSON, plain text) passes through. Runs on the agent's
// tool thread — no GTK, no UI marshalling.
class WebFetchTool : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "web_fetch"; }

    std::string get_description() const override {
        return "Fetch a URL over HTTP(S) without opening a browser tab and "
               "return its content as text (HTML pages are converted to "
               "plain text). Use for static documentation and reference "
               "pages; use app_ui_control browser.open/read instead for "
               "JavaScript-rendered pages.";
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
                 {{"url",
                   {{"type", "string"},
                    {"description", "http:// or https:// URL to GET"}}},
                  {"max_chars",
                   {{"type", "integer"},
                    {"description",
                     "truncate the returned text (default 8000, max "
                     "20000)"}}}}},
                {"required", nlohmann::json::array({"url"})}}}}},
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext&) override;
};

} // namespace monkeybyte
