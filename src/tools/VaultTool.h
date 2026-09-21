#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "BaseTool.h"

namespace daedalus { class SecretsVault; }

using json = nlohmann::json;

/**
 * VaultTool — §TASK-2026-09-21-01: agent-facing front-end to
 * daedalus::SecretsVault (the user-global encrypted secrets store).
 * The vault is the AGENT's password manager: it can create, read, edit
 * and delete entries autonomously.
 *
 * One tool, five actions:
 *   vault(action="list")                 → metadata for every entry;
 *                                          NEVER secret values.
 *   vault(action="get", name=...)        → decrypted fields for one entry.
 *   vault(action="store", name=...,      → create/replace a whole entry;
 *         kind=..., description=...,       confirms WITHOUT echoing values.
 *         fields={...})
 *   vault(action="edit", name=...,       → partial update: merge the given
 *         fields={...})                    fields into the existing entry —
 *                                          no get-first needed, so the other
 *                                          secrets never enter the context.
 *   vault(action="delete", name=...)     → remove one entry.
 *
 * The vault is GLOBAL (SecretsVault::global()): the same store is shared
 * by every project/agent on this machine. `get` results enter the
 * conversation history — hosts that don't want that can disable the tool
 * via AgentConfig::tools_disabled.
 *
 * The action handlers are public statics taking an explicit vault so
 * tests can drive them against a temp-home vault hermetically.
 */
class VaultTool : public BaseTool {
public:
    VaultTool();

    std::string get_name() const override { return name_; }
    std::string get_description() const override { return description_; }
    json get_definition() const override;
    json execute(const json& args, FileIO& file_io) override;

    // Action handlers — static, vault injected. Used by execute() with
    // SecretsVault::global(); directly testable with a temp vault.
    static json do_list(daedalus::SecretsVault& vault);
    static json do_get(daedalus::SecretsVault& vault, const json& args);
    static json do_store(daedalus::SecretsVault& vault, const json& args);
    static json do_edit(daedalus::SecretsVault& vault, const json& args);
    static json do_delete(daedalus::SecretsVault& vault, const json& args);

private:
    std::string name_;
    std::string description_;
};
