// SPDX-License-Identifier: BSL-1.0
// src/tools/BrainTool.h
//
// §SECOND-BRAIN: agent-facing front-end to daedalus::SecondBrain — the
// GLOBAL plain-markdown knowledge base shared by every agent on this
// machine. One tool, action-dispatched (the §TASK-2026-09-21-01 VaultTool
// pattern: a single compact schema riding on every request beats seven
// separate tool definitions).
//
// Actions:
//   brain(action="capture", folder?, title?, tags?, body)   → create note
//   brain(action="search", query, limit?)                   → FTS5 hits
//   brain(action="read", id)                                → full note
//   brain(action="list", folder?, tag?, limit?)             → metadata
//   brain(action="update", id, title?, tags?, body?)        → edit fields
//   brain(action="append", id, markdown)                    → add to body
//   brain(action="file", id, folder)                        → PARA re-file
//   brain(action="backlinks", id)                           → referrers
//   brain(action="delete", id)                              → remove note
//   brain(action="reindex")                                 → rebuild index
//
// The brain is GLOBAL (SecondBrain::global()): one knowledge base for all
// projects. The markdown files under <user_data_root>/brain/ are the
// source of truth; the FTS5 index is a rebuildable cache (reindex).
//
// The action handlers are public statics taking an explicit SecondBrain
// so tests can drive them against a temp-home brain hermetically.

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

namespace daedalus { class SecondBrain; }

#include "daedalus/settings.h"  // SettingsStore::BrainNoteRow (nested — no fwd-decl)

using json = nlohmann::json;

class BrainTool : public BaseTool {
public:
    BrainTool();

    std::string get_name() const override { return name_; }
    std::string get_description() const override { return description_; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;

    // Action handlers — public statics for hermetic tests (cf. VaultTool).
    static json do_capture(daedalus::SecondBrain& brain, const json& args);
    static json do_search(daedalus::SecondBrain& brain, const json& args);
    static json do_read(daedalus::SecondBrain& brain, const json& args);
    static json do_list(daedalus::SecondBrain& brain, const json& args);
    static json do_update(daedalus::SecondBrain& brain, const json& args);
    static json do_append(daedalus::SecondBrain& brain, const json& args);
    static json do_file(daedalus::SecondBrain& brain, const json& args);
    static json do_backlinks(daedalus::SecondBrain& brain, const json& args);
    static json do_delete(daedalus::SecondBrain& brain, const json& args);
    static json do_reindex(daedalus::SecondBrain& brain);

private:
    /// Shared serialiser so list/search render notes the same way.
    static json meta_json(const daedalus::SettingsStore::BrainNoteRow& m);

    std::string name_;
    std::string description_;
};
