// SPDX-License-Identifier: BSL-1.0
// src/tools/ListToolsTool.h
//
// §LIST-TOOLS-CORE (2026-09-21): agent-visible live tool roster.
//
// Why this exists: the tool definitions ARE in context twice (the API
// `tools` parameter on every request, and a JSON schema block in the
// system prompt), but the system-prompt copy is composed ONCE — pinned
// as messages[0] on the first turn and never rebuilt. On session resume
// it comes back stale from messages.json, and tools/MCP servers that
// appeared later never enter it. Compression preserves messages[0], so
// the model doesn't "forget" — the roster just drifts out of date and,
// with large MCP schema blocks, out of attention.
//
// This tool is the self-service fix: at any point the agent can ask
// "what tools do I actually have right now?" and get the LIVE registry
// (core + host-registered + MCP-proxied), not the session-start snapshot.
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

using json = nlohmann::json;

class ListToolsTool : public BaseTool {
public:
    std::string get_name() const override { return "list_tools"; }
    std::string get_description() const override {
        return "List the tools currently registered in this agent process "
               "(core, host-registered, and MCP-proxied), with a one-line "
               "description each. Use this when you are unsure what tools "
               "you have, after MCP servers connect, or when a resumed "
               "session's tool list may be stale. Optional 'filter' "
               "narrows by name/description substring; 'verbose' returns "
               "the full JSON schemas. Note: your host may have disabled "
               "individual tools — a listed tool that fails with "
               "'disabled by host policy' is one of those.";
    }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;
};
