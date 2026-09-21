#include "tools/McpProxiedTool.h"
#include "mcp/McpServerManager.h"

#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

McpProxiedTool::McpProxiedTool(std::string server_name, mcp::ToolDefinition def,
                                   mcp::McpServerManager* mcp_manager)
    : server_name_(std::move(server_name)),
      def_(std::move(def)),
      mcp_manager_(mcp_manager) {}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

std::string McpProxiedTool::get_name() const {
    // Canonical MCP-host namespace: mcp__<server>__<tool>
    return "mcp__" + sanitize_name(server_name_) + "__" + sanitize_name(def_.name);
}

std::string McpProxiedTool::get_description() const {
    return def_.description;
}

namespace {

// §MCP-SCHEMA-SANITIZE (2026-09-21): normalise a server-published JSON
// Schema into the subset strict providers accept. Moonshot ("moonshot
// flavored json schema") answers EVERY chat request with HTTP 400 when a
// tool schema carries:
//   - anyOf/oneOf combinators — including the pydantic Optional pattern
//     [T, {"type":"null"}] and $ref branches ("detected infinite
//     recursion without termination condition"),
//   - an object-valued additionalProperties ("must be a boolean or an
//     object" — a schema object is rejected in practice),
//   - unresolved local $ref / top-level $defs.
// The schema only steers the MODEL; the wire call is never validated
// against it, so rewriting is safe. Rewrites:
//   anyOf/oneOf  → drop {"type":"null"} branches (optionality is already
//                  expressed by absence from "required"); one branch left
//                  → inline it; several left → keep the first (the other
//                  forms remain usable server-side).
//   $ref "#/$defs/X" | "#/definitions/X" → inline the definition.
//   additionalProperties: <schema> → true.
// Top-level $defs/definitions are dropped once refs are inlined.
constexpr int kSanitizeMaxDepth = 16;

json sanitize_schema_node(const json& node_in, const json& defs, int depth) {
    if (!node_in.is_object()) return node_in;
    json node = node_in;

    // --- $ref inlining (local definitions only) -------------------------
    auto ref_it = node.find("$ref");
    if (ref_it != node.end() && ref_it->is_string()) {
        const std::string ref = ref_it->get<std::string>();
        std::string key;
        for (const char* prefix : {"#/$defs/", "#/definitions/"}) {
            const std::string p = prefix;
            if (ref.rfind(p, 0) == 0) { key = ref.substr(p.size()); break; }
        }
        json resolved;
        if (depth < kSanitizeMaxDepth && !key.empty() && defs.contains(key)) {
            resolved = sanitize_schema_node(defs[key], defs, depth + 1);
        }  // else: unresolvable / cyclic → empty schema (accepts anything).
        for (const auto& [k, v] : node.items()) {
            if (k != "$ref" && !resolved.contains(k)) resolved[k] = v;
        }
        node = std::move(resolved);
    }

    // --- anyOf / oneOf collapse -----------------------------------------
    for (const char* comb : {"anyOf", "oneOf"}) {
        auto comb_it = node.find(comb);
        if (comb_it == node.end() || !comb_it->is_array()) continue;

        json branches = json::array();
        for (const auto& b : *comb_it) {
            // Pydantic Optional marker: {"type":"null"} carries no
            // information the model needs.
            if (b.is_object() && b.value("type", "") == "null") continue;
            branches.push_back(sanitize_schema_node(b, defs, depth + 1));
        }
        json chosen = branches.empty() ? json::object() : branches.front();
        // Sibling metadata (title / description / default / ...) survives.
        for (const auto& [k, v] : node.items()) {
            if (k == comb || chosen.contains(k)) continue;
            chosen[k] = v;
        }
        node = std::move(chosen);
        break;  // one combinator handled; a second is pathological
    }

    // --- additionalProperties: schema → boolean -------------------------
    auto ap_it = node.find("additionalProperties");
    if (ap_it != node.end() && !ap_it->is_boolean()) {
        node["additionalProperties"] = true;
    }

    // --- recurse into child schemas -------------------------------------
    auto props_it = node.find("properties");
    if (props_it != node.end() && props_it->is_object()) {
        for (auto& [pk, pv] : props_it->items()) {
            pv = sanitize_schema_node(pv, defs, depth + 1);
        }
    }
    auto items_it = node.find("items");
    if (items_it != node.end() && items_it->is_object()) {
        *items_it = sanitize_schema_node(*items_it, defs, depth + 1);
    }
    return node;
}

json sanitize_tool_parameters(const json& input_schema) {
    if (!input_schema.is_object()) return json::object();

    // Collect local definitions for $ref resolution.
    json defs = json::object();
    for (const char* dk : {"$defs", "definitions"}) {
        auto it = input_schema.find(dk);
        if (it != input_schema.end() && it->is_object()) {
            for (const auto& [k, v] : it->items()) defs[k] = v;
        }
    }

    json out = sanitize_schema_node(input_schema, defs, 0);
    out.erase("$defs");
    out.erase("definitions");
    return out;
}

}  // namespace

json McpProxiedTool::get_definition() const {
    // OpenAI-compatible function schema. The model's tool dispatcher sees
    // the same shape it would for any other native tool, with the server's
    // own inputSchema — normalised by §MCP-SCHEMA-SANITIZE so strict
    // providers (Moonshot) don't reject the whole request — as
    // `parameters`.
    const json parameters = sanitize_tool_parameters(def_.input_schema);

    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name",        get_name()},
            {"description", def_.description},
            {"parameters",  parameters}
        })}
    });
}

json McpProxiedTool::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

json McpProxiedTool::execute(const json& args,
                             [[maybe_unused]] FileIO& file_io,
                             ToolProgressFn progress) {
    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {
            {"stage",   stage},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };

    // §Phase 11: uses injected mcp_manager_ instead of ::instance().
    // If mcp_manager_ is nullptr (shouldn't happen in practice), return error.
    if (!mcp_manager_) {
        return json::object({
            {"status",  "error"},
            {"server",  server_name_},
            {"tool",    def_.name},
            {"message",  "MCP manager not configured (mcp_enabled=false)"}
        });
    }
    // §Phase C2: emit "calling" before the MCP round-trip. This is the
    // stage that has the most latency variance — stdio servers spawn a
    // child process on first call (often 1-2s), HTTP servers do a
    // network round-trip, etc. Without this event the UI is frozen
    // until the result comes back.
    emit_progress("calling", 0,
                  "Calling " + server_name_ + "::" + def_.name);
    mcp::ToolResult result =
        mcp_manager_->call_tool(server_name_, def_.name, args);

    // §Phase C2: emit "parsing" once the server's response is back. The
    // 90% milestone is intentional — result shaping (content extraction,
    // error mapping) is fast but not free, and reserving the last 10%
    // for the implicit "done" signal that tool_result provides.
    emit_progress("parsing", 90, "Parsing response");

    json response = json::object({
        {"status", result.success ? "success" : "error"},
        {"server", server_name_},
        {"tool",   def_.name}
    });

    if (result.success) {
        response["result"] = result.content;
    } else {
        response["message"] = result.error.value_or("Unknown error");
    }

    return response;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string McpProxiedTool::sanitize_name(const std::string& s) {
    // OpenAI / Anthropic tool names allow letters, digits, underscores and
    // dashes. Anything else gets squashed to '_' so the namespace stays
    // parseable and the underlying routing is unambiguous.
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        const bool ok =
            std::isalnum(uc) || c == '_' || c == '-';
        out.push_back(ok ? c : '_');
    }
    return out;
}