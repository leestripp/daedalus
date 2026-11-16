#pragma once

#include <daedalus/types/BaseTool.h>

namespace monkeybyte {

// daedalus host tool exposing the UiControl surface to the in-app agent.
// The agent can move sizers, switch panels, open files, preview documents,
// open URLs in browser tabs, navigate and read back the live page text,
// and read geometry.
class AppUiTool : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "app_ui_control"; }

    std::string get_description() const override {
        return "Control the MonkeyByte application UI: move panel sizers, "
               "switch sidebar panels, set panel widths, open files in the "
               "editor, open markdown/HTML previews of workspace files, open "
               "a URL (e.g. a local dev server you started) in a browser tab "
               "to show web-app progress, then navigate it, read the live "
               "page text back (works on JavaScript-rendered pages), run "
               "JavaScript in the page (browser.eval — DOM state, clicks, "
               "form fills), list its links (browser.links), and read "
               "captured console/page errors (browser.console) for "
               "debugging, and list/select responsive device viewport "
               "presets (browser.device) for layout testing; read the "
               "current editor text selection, and read "
               "window geometry. editor.open_as opens a custom read-only "
               "Markdown/HTML preview viewer, NOT a browser tab. browser.* "
               "commands operate only on browser tabs created by browser.open "
               "and accept an optional {tab: url} arg to target one.";
    }

    nlohmann::json get_definition() const override {
        nlohmann::json properties;
        properties["command"] = {
            {"type", "string"},
            {"description",
             "sizer.begin | sizer.delta | sizer.end | panel.width | "
             "panel.switch | editor.open | editor.open_as | browser.open | "
             "browser.navigate | browser.read | browser.eval | "
             "browser.links | browser.console | browser.upload | "
             "browser.device | browser.list | browser.close | "
             "editor.selection | "
             "app.geometry | app.commands"},
        };
        properties["args"] = {
            {"type", "object"},
            {"description",
             "Command arguments, e.g. {target:'sidebar',dx:40}, "
             "{path:'/abs/file'}, {path:'/abs/file',line:42}, "
             "{path:'/abs/file.md',mode:'markdown'}, "
             "{url:'http://localhost:3000'}, "
             "{selector:'main',max_chars:4000} for browser.read, "
             "{script:'document.title'} for browser.eval, "
             "{max_entries:50,clear:true} for browser.console, "
             "{} for browser.device preset list/current size, "
             "{preset:'iphone-14-15',rotate:false} to select a viewport, "
             "{fit:true} to fill the tab, "
             "{path:'/abs/file'} for browser.upload (queue file, then "
             "click the page's file input via browser.eval)"},
        };
        return {
            {"type", "function"},
            {"function",
             {{"name", get_name()},
              {"description", get_description()},
              {"parameters",
               {{"type", "object"},
                {"properties", properties},
                {"required", nlohmann::json::array({"command"})}}}}},
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext&) override;
};

} // namespace monkeybyte
