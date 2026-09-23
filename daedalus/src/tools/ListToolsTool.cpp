// SPDX-License-Identifier: BSL-1.0
// src/tools/ListToolsTool.cpp — see header for §LIST-TOOLS-CORE rationale.

#include "tools/ListToolsTool.h"

#include "tools/ToolHandler.h"
#include "tools/ToolFactory.h"

#include <algorithm>
#include <cctype>

namespace {

// Case-insensitive substring match (roster filter).
bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](unsigned char a, unsigned char b) {
                              return std::tolower(a) == std::tolower(b);
                          });
    return it != haystack.end();
}

// Keep the compact roster compact: some tool descriptions (MCP-proxied
// ones in particular) are multi-paragraph essays.
std::string shorten(const std::string& desc, size_t max_len = 120) {
    // First line only — MCP tool descriptions often embed blank-line
    // separated usage essays after the summary sentence.
    std::string first = desc.substr(0, desc.find('\n'));
    // Collapse runs of whitespace so the markdown table stays one line.
    std::string out;
    out.reserve(first.size());
    bool last_space = false;
    for (char c : first) {
        if (c == ' ' || c == '\t') {
            if (!last_space) out += ' ';
            last_space = true;
        } else {
            out += c;
            last_space = false;
        }
    }
    if (out.size() > max_len) {
        out = out.substr(0, max_len - 3) + "...";
    }
    return out;
}

} // namespace

json ListToolsTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"filter", {
                        {"type", "string"},
                        {"description", "Optional case-insensitive substring "
                                        "matched against tool name and "
                                        "description (e.g. \"memory\", \"bash\", "
                                        "\"mcp__comfy\")."}
                    }},
                    {"verbose", {
                        {"type", "boolean"},
                        {"description", "When true, return the full JSON "
                                        "schemas of the matching tools "
                                        "instead of the compact roster. "
                                        "Default false."}
                    }}
                }},
                {"required", json::array()}
            }}
        }}
    };
}

json ListToolsTool::execute(const json& args, FileIO& /*file_io*/) {
    const std::string filter = args.value("filter", "");
    const bool verbose = args.value("verbose", false);

    auto factory = ToolHandler::get_tool_factory();
    if (!factory) {
        return json::object({
            {"status", "error"},
            {"message", "Tool factory is not initialized in this process."}
        });
    }

    const json all = factory->get_all_definitions();

    json matched = json::array();
    for (const auto& def : all) {
        if (!def.contains("function") || !def["function"].is_object()) continue;
        const auto& fn = def["function"];
        const std::string name = fn.value("name", "");
        const std::string desc = fn.value("description", "");
        if (!filter.empty() && !icontains(name, filter) && !icontains(desc, filter)) {
            continue;
        }
        matched.push_back(def);
    }

    if (verbose) {
        return json::object({
            {"status", "success"},
            {"count", matched.size()},
            {"tools", matched}
        });
    }

    // Compact roster: markdown table, name + shortened description.
    std::string roster;
    size_t core_count = 0, mcp_count = 0;
    for (const auto& def : matched) {
        const auto& fn = def["function"];
        const std::string name = fn.value("name", "");
        const std::string desc = shorten(fn.value("description", ""));
        if (name.rfind("mcp__", 0) == 0) ++mcp_count; else ++core_count;
        roster += "| `" + name + "` | " + desc + " |\n";
    }

    return json::object({
        {"status", "success"},
        {"count", matched.size()},
        {"core_tools", core_count},
        {"mcp_tools", mcp_count},
        {"filter", filter},
        {"roster", "| Tool | Description |\n|------|-------------|\n" + roster},
        {"hint", "Pass verbose=true for full JSON schemas of these tools, "
                 "or filter=\"<text>\" to narrow the list."}
    });
}
