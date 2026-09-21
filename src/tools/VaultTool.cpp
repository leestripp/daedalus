#include "VaultTool.h"
#include "../core/FileIO.h"

#include <daedalus/vault.h>

#include <stdexcept>

VaultTool::VaultTool() {
    name_ = "vault";
    description_ =
        "Your personal encrypted password manager — GLOBAL across all "
        "projects, shared by every agent on this machine. Use it to "
        "create, read, edit and delete credentials: logins, secure "
        "notes, API keys, etc. Proactively offer to save credentials "
        "the user shares. Actions: 'list' (metadata only, never secret "
        "values), 'get' (decrypt one entry), 'store' (create/replace a "
        "whole entry), 'edit' (merge fields into an existing entry — no "
        "get-first needed), 'delete' (remove an entry).";
}

json VaultTool::get_definition() const {
    return {
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", {
                {"type", "object"},
                {"properties", {
                    {"action", {
                        {"type", "string"},
                        {"enum", {"list", "get", "store", "edit", "delete"}},
                        {"description", "list: metadata for all entries (no secret values). get: decrypt one entry. store: create/replace a whole entry. edit: merge fields into an existing entry. delete: remove an entry."}
                    }},
                    {"name", {
                        {"type", "string"},
                        {"description", "Entry name, e.g. 'github/api-token' or 'prod-db-login'. Required for get/store/edit/delete."}
                    }},
                    {"kind", {
                        {"type", "string"},
                        {"enum", {"login", "note", "api_key", "custom"}},
                        {"description", "Entry kind (store/edit only; default 'custom'). Conventional fields: login={username,password,url}, note={text}, api_key={key,service}."}
                    }},
                    {"description", {
                        {"type", "string"},
                        {"description", "Plaintext description shown by list (store/edit only). Do NOT put secrets here."}
                    }},
                    {"fields", {
                        {"type", "object"},
                        {"additionalProperties", {"type", "string"}},
                        {"description", "Secret fields to encrypt (store/edit), e.g. {\"key\": \"sk-...\"}. store replaces the whole field set; edit merges. Values are never echoed back."}
                    }}
                }},
                {"required", {"action"}}
            }}
        }}
    };
}

json VaultTool::execute(const json& args, FileIO& /*file_io*/) {
    const std::string action = args.value("action", "");
    try {
        auto vault = daedalus::SecretsVault::global();
        if (action == "list")   return do_list(vault);
        if (action == "get")    return do_get(vault, args);
        if (action == "store")  return do_store(vault, args);
        if (action == "edit")   return do_edit(vault, args);
        if (action == "delete") return do_delete(vault, args);
        return json::object({
            {"status", "error"},
            {"message", "vault: unknown or missing action '" + action +
                        "' — expected list | get | store | edit | delete"},
        });
    } catch (const std::exception& e) {
        // Crypto/IO errors (tamper, wrong key, unreadable vault.key)
        // surface as tool errors, never as crashes.
        return json::object({{"status", "error"}, {"message", e.what()}});
    }
}

json VaultTool::do_list(daedalus::SecretsVault& vault) {
    const auto metas = vault.list();
    json entries = json::array();
    for (const auto& m : metas) {
        // Metadata only — secret fields are never touched here.
        entries.push_back(json::object({
            {"name", m.name},
            {"kind", m.kind},
            {"description", m.description},
            {"created_at", m.created_at},
            {"updated_at", m.updated_at},
        }));
    }
    return json::object({
        {"status", "success"},
        {"entries", std::move(entries)},
        {"count", metas.size()},
    });
}

json VaultTool::do_get(daedalus::SecretsVault& vault, const json& args) {
    const std::string name = args.value("name", "");
    if (name.empty()) {
        return json::object({{"status", "error"},
                             {"message", "vault get: 'name' is required"}});
    }
    auto entry = vault.get(name);
    if (!entry) {
        return json::object({
            {"status", "error"},
            {"message", "vault: no entry named '" + name +
                        "' — try action='list' to see what exists"},
        });
    }
    return json::object({
        {"status", "success"},
        {"name", entry->name},
        {"kind", entry->kind},
        {"description", entry->description},
        {"fields", entry->fields},
        {"created_at", entry->created_at},
        {"updated_at", entry->updated_at},
        {"note", "These decrypted values are now in the conversation "
                 "history — prefer short-lived use."},
    });
}

namespace {
// Shared store/edit argument extraction. Returns false (and sets err)
// when the args are unusable.
bool extract_entry_args(const json& args, daedalus::VaultEntry& entry,
                        json& err, bool require_fields) {
    entry.name = args.value("name", "");
    if (entry.name.empty()) {
        err = json::object({{"status", "error"},
                            {"message", "vault: 'name' is required"}});
        return false;
    }
    if (require_fields &&
        (!args.contains("fields") || !args["fields"].is_object() ||
         args["fields"].empty())) {
        err = json::object({{"status", "error"},
                            {"message", "vault: 'fields' must be a non-empty "
                                        "object of string values"}});
        return false;
    }
    entry.kind        = args.value("kind", "");
    entry.description = args.value("description", "");
    if (args.contains("fields") && args["fields"].is_object()) {
        for (const auto& [k, v] : args["fields"].items()) {
            entry.fields[k] = v.is_string() ? v.get<std::string>() : v.dump();
        }
    }
    return true;
}

json confirm_write(const daedalus::VaultEntry& entry,
                   const char* what) {
    // Confirm WITHOUT echoing any secret value — list field keys only.
    json field_names = json::array();
    for (const auto& [k, v] : entry.fields) field_names.push_back(k);
    return json::object({
        {"status", "success"},
        {"name", entry.name},
        {"action_done", what},
        {"field_names", std::move(field_names)},
    });
}
}  // namespace

json VaultTool::do_store(daedalus::SecretsVault& vault, const json& args) {
    daedalus::VaultEntry entry;
    json err;
    if (!extract_entry_args(args, entry, err, /*require_fields=*/true)) {
        return err;
    }
    if (entry.kind.empty()) entry.kind = "custom";
    const bool updating = vault.exists(entry.name);
    vault.store(entry);
    return confirm_write(entry, updating ? "replaced" : "created");
}

json VaultTool::do_edit(daedalus::SecretsVault& vault, const json& args) {
    daedalus::VaultEntry entry;
    json err;
    // fields optional only when kind/description is being changed.
    const bool touches_meta = args.contains("kind") || args.contains("description");
    if (!extract_entry_args(args, entry, err, /*require_fields=*/!touches_meta)) {
        return err;
    }
    if (entry.fields.empty() && !touches_meta) {
        return json::object({{"status", "error"},
                             {"message", "vault edit: nothing to change — "
                                         "provide 'fields', 'kind' and/or "
                                         "'description'"}});
    }
    if (!vault.edit(entry.name, entry.fields, entry.kind, entry.description)) {
        return json::object({
            {"status", "error"},
            {"message", "vault: no entry named '" + entry.name +
                        "' — use action='store' to create it"},
        });
    }
    return confirm_write(entry, "edited");
}

json VaultTool::do_delete(daedalus::SecretsVault& vault, const json& args) {
    const std::string name = args.value("name", "");
    if (name.empty()) {
        return json::object({{"status", "error"},
                             {"message", "vault delete: 'name' is required"}});
    }
    if (!vault.exists(name)) {
        return json::object({{"status", "error"},
                             {"message", "vault: no entry named '" + name + "'"}});
    }
    vault.remove(name);
    return json::object({{"status", "success"},
                         {"deleted", name}});
}
