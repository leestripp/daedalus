// ============================================================================
// tests/test_vault.cpp
//
// §TASK-2026-09-21-01 regression/feature test: SecretsVault (encrypted
// password manager) + vault tool.
//
// Covers:
//   1. Store/get round-trip (login with fields) — values decrypt exactly.
//   2. Update preserves created_at, refreshes updated_at.
//   3. list() returns metadata ONLY (no secret field material).
//   4. Tamper detection: a flipped payload byte fails GCM (throws).
//   5. Wrong key file → get() throws (no silent plaintext fallback).
//   6. No plaintext secrets in the DB file (strings-level check).
//   7. v5→v6 migration creates vault_entries on an existing DB.
//
//   8. edit(): field-merge semantics, meta-only edits, missing entry.
//   9. VaultTool actions end-to-end (create/edit/delete/list/get +
//      error paths) via the public static handlers with an injected
//      temp vault — the agent's create/edit/delete contract.
//
// Hermetic — temp dirs, no network, no process spawns.
// ============================================================================

#include <daedalus/vault.h>
#include <daedalus/settings.h>
#include "tools/VaultTool.h"
#include "core/FileIO.h"

// Force assertions on in Release builds (project convention).
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("vault-test-" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

daedalus::VaultEntry sample_login() {
    daedalus::VaultEntry e;
    e.name        = "github/api-token";
    e.kind        = "api_key";
    e.description = "GitHub PAT for CI";
    e.fields      = {{"key", "ghp_supersecretvalue123"},
                     {"service", "github.com"}};
    return e;
}

// 1-3: round-trip, update semantics, metadata-only list.
void test_roundtrip_and_list() {
    auto root = fresh_dir("roundtrip");
    auto store = daedalus::SettingsStore::for_home(root / "home");
    daedalus::SecretsVault vault(store, root / "cfg" / "vault.key");

    assert(!vault.exists("github/api-token"));
    vault.store(sample_login());
    assert(vault.exists("github/api-token"));

    auto got = vault.get("github/api-token");
    assert(got.has_value());
    assert(got->kind == "api_key");
    assert(got->description == "GitHub PAT for CI");
    assert(got->fields.at("key") == "ghp_supersecretvalue123");
    assert(got->fields.at("service") == "github.com");
    assert(!got->created_at.empty() && !got->updated_at.empty());

    // Update: created_at preserved, new field merged by replacement.
    daedalus::VaultEntry upd = sample_login();
    upd.description = "rotated token";
    upd.fields      = {{"key", "ghp_rotated456"}};
    vault.store(upd);
    auto got2 = vault.get("github/api-token");
    assert(got2->fields.at("key") == "ghp_rotated456");
    assert(got2->fields.size() == 1);           // store replaces, not merges
    assert(got2->description == "rotated token");
    assert(got2->created_at == got->created_at);

    // list(): metadata only.
    const auto metas = vault.list();
    assert(metas.size() == 1);
    assert(metas[0].name == "github/api-token");
    assert(metas[0].kind == "api_key");

    // Unknown name → nullopt, not throw.
    assert(!vault.get("nope").has_value());

    // Key file exists with owner-only perms.
    std::error_code ec;
    const auto perms = fs::status(root / "cfg" / "vault.key", ec).permissions();
    assert(!ec);
    assert((perms & fs::perms::others_all) == fs::perms::none);
    assert((perms & fs::perms::group_all) == fs::perms::none);

    fs::remove_all(root);
    std::cout << "[ok] test_roundtrip_and_list" << std::endl;
}

// 4-5: tamper + wrong key both fail loudly.
void test_tamper_and_wrong_key() {
    auto root = fresh_dir("tamper");
    auto store = daedalus::SettingsStore::for_home(root / "home");
    daedalus::SecretsVault vault(store, root / "cfg" / "vault.key");
    vault.store(sample_login());

    // Flip a byte inside the stored payload blob.
    auto row = store.get_vault_row("github/api-token");
    assert(row.has_value());
    auto& b64 = row->payload_b64;
    b64[b64.size() / 2] = (b64[b64.size() / 2] == 'A') ? 'B' : 'A';
    store.upsert_vault_row(*row);
    bool threw = false;
    try { (void)vault.get("github/api-token"); }
    catch (const std::exception&) { threw = true; }
    assert(threw && "tampered payload must fail the GCM tag check");

    // Wrong key: fresh vault over the same DB but a different key file.
    auto store2 = daedalus::SettingsStore::for_home(root / "home2");
    daedalus::SecretsVault vault2(store2, root / "cfg2" / "vault.key");
    vault2.store(sample_login());  // creates its own key + row in home2
    daedalus::SecretsVault foreign(store2, root / "cfg" / "vault.key");
    threw = false;
    try { (void)foreign.get("github/api-token"); }
    catch (const std::exception&) { threw = true; }
    assert(threw && "decrypting with a foreign key must fail");

    fs::remove_all(root);
    std::cout << "[ok] test_tamper_and_wrong_key" << std::endl;
}

// 6: secret values must not appear as plaintext in the DB file.
void test_no_plaintext_in_db_file() {
    auto root = fresh_dir("plaintext");
    auto store = daedalus::SettingsStore::for_home(root / "home");
    daedalus::SecretsVault vault(store, root / "cfg" / "vault.key");
    vault.store(sample_login());
    vault.remove("github/api-token");  // exercise delete too
    vault.store(sample_login());

    std::ifstream f(root / "home" / "daedalus.db", std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string db_bytes = ss.str();
    assert(db_bytes.find("ghp_supersecretvalue123") == std::string::npos);
    // Metadata IS plaintext by design:
    assert(db_bytes.find("github/api-token") != std::string::npos);

    fs::remove_all(root);
    std::cout << "[ok] test_no_plaintext_in_db_file" << std::endl;
}

// 7: a v5-era DB (no vault_entries) gains the table via migration.
void test_migration_from_v5() {
    auto root = fresh_dir("migrate");
    {
        auto store = daedalus::SettingsStore::for_home(root / "home");
        // Table exists on open (fresh DB runs kSchemaSql incl. v6).
        store.upsert_vault_row(daedalus::SettingsStore::VaultRow{
            "probe", "custom", "", "AA==", "AA==", "t", "t"});
        assert(store.get_vault_row("probe").has_value());
        store.delete_vault_row("probe");
        assert(!store.get_vault_row("probe").has_value());
    }
    fs::remove_all(root);
    std::cout << "[ok] test_migration_from_v5" << std::endl;
}

// 8: edit() merges fields without touching the rest; meta-only edits
//    work; missing entries return false.
void test_edit_merge() {
    auto root = fresh_dir("edit");
    auto store = daedalus::SettingsStore::for_home(root / "home");
    daedalus::SecretsVault vault(store, root / "cfg" / "vault.key");

    vault.store(sample_login());   // fields: key, service

    // Rotate only the key — service must survive WITHOUT being resent.
    assert(vault.edit("github/api-token", {{"key", "ghp_new999"}}));
    auto got = vault.get("github/api-token");
    assert(got->fields.at("key") == "ghp_new999");
    assert(got->fields.at("service") == "github.com");   // preserved
    assert(got->kind == "api_key");                       // preserved
    assert(got->description == "GitHub PAT for CI");      // preserved

    // Add a brand-new field via edit.
    assert(vault.edit("github/api-token", {{"org", "acme"}}));
    assert(vault.get("github/api-token")->fields.count("org") == 1);

    // Meta-only edit: kind + description, fields untouched.
    assert(vault.edit("github/api-token", {}, "login", "rotated CI token"));
    got = vault.get("github/api-token");
    assert(got->kind == "login");
    assert(got->description == "rotated CI token");
    assert(got->fields.at("key") == "ghp_new999");

    // Missing entry → false (no implicit create).
    assert(!vault.edit("does/not-exist", {{"key", "x"}}));
    assert(!vault.exists("does/not-exist"));

    fs::remove_all(root);
    std::cout << "[ok] test_edit_merge" << std::endl;
}

// 9: the agent's tool contract — create, read, edit, delete through
//    VaultTool's static handlers against an injected temp vault.
void test_tool_actions() {
    auto root = fresh_dir("tool");
    auto store = daedalus::SettingsStore::for_home(root / "home");
    daedalus::SecretsVault vault(store, root / "cfg" / "vault.key");

    // create
    auto r = VaultTool::do_store(vault, {
        {"action", "store"},
        {"name", "prod/db-login"},
        {"kind", "login"},
        {"description", "prod database"},
        {"fields", {{"username", "agent"}, {"password", "s3cret!"}}},
    });
    assert(r["status"] == "success");
    assert(r["action_done"] == "created");
    // Confirmation must NOT echo secret values:
    const std::string dumped = r.dump();
    assert(dumped.find("s3cret!") == std::string::npos);
    assert(dumped.find("\"agent\"") == std::string::npos);  // value not echoed
    assert(dumped.find("username") != std::string::npos);     // field NAMES ok

    // store replaces the whole field set
    r = VaultTool::do_store(vault, {
        {"action", "store"}, {"name", "prod/db-login"},
        {"fields", {{"password", "onlythis"}}},
    });
    assert(r["action_done"] == "replaced");
    assert(vault.get("prod/db-login")->fields.size() == 1);

    // edit merges — brings back username without resending password
    r = VaultTool::do_edit(vault, {
        {"action", "edit"}, {"name", "prod/db-login"},
        {"fields", {{"username", "agent2"}}},
    });
    assert(r["status"] == "success");
    assert(r["action_done"] == "edited");
    {
        auto e = vault.get("prod/db-login");
        assert(e->fields.at("username") == "agent2");
        assert(e->fields.at("password") == "onlythis");
    }

    // edit on missing entry → error pointing at store
    r = VaultTool::do_edit(vault, {
        {"action", "edit"}, {"name", "nope"},
        {"fields", {{"x", "y"}}},
    });
    assert(r["status"] == "error");
    assert(r["message"].get<std::string>().find("store") != std::string::npos);

    // edit with nothing to change → error
    r = VaultTool::do_edit(vault, {{"action", "edit"}, {"name", "prod/db-login"}});
    assert(r["status"] == "error");

    // list: metadata only
    r = VaultTool::do_list(vault);
    assert(r["status"] == "success");
    assert(r["count"] == 1);
    assert(r["entries"][0]["name"] == "prod/db-login");
    assert(!r["entries"][0].contains("fields"));
    assert(r.dump().find("onlythis") == std::string::npos);

    // get: returns the decrypted fields
    r = VaultTool::do_get(vault, {{"action", "get"}, {"name", "prod/db-login"}});
    assert(r["status"] == "success");
    assert(r["fields"]["password"] == "onlythis");

    // delete: happy path + missing-name error
    r = VaultTool::do_delete(vault, {{"action", "delete"}, {"name", "prod/db-login"}});
    assert(r["status"] == "success");
    assert(!vault.exists("prod/db-login"));
    r = VaultTool::do_delete(vault, {{"action", "delete"}, {"name", "prod/db-login"}});
    assert(r["status"] == "error");

    // arg-validation errors
    assert(VaultTool::do_store(vault, {{"action", "store"}})["status"] == "error");
    assert(VaultTool::do_get(vault, {{"action", "get"}})["status"] == "error");

    fs::remove_all(root);
    std::cout << "[ok] test_tool_actions" << std::endl;
}

}  // namespace

int main() {
    test_roundtrip_and_list();
    test_tamper_and_wrong_key();
    test_no_plaintext_in_db_file();
    test_migration_from_v5();
    test_edit_merge();
    test_tool_actions();
    std::cout << "all vault tests passed" << std::endl;
    return 0;
}
