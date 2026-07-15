// SPDX-License-Identifier: BSL-1.0
// tests/test_settings_migrator.cpp
//
// Unit tests for the v0.3.1 JSON → SQLite auto-migrator.
//
// What we pin:
//   M1. needs_migration() returns false when no legacy JSON exists.
//   M2. needs_migration() returns false when the store already has data
//       (idempotency — second run is a no-op).
//   M3. needs_migration() returns true when JSON exists and store is empty.
//   M4. run() copies a model from config.json into the store.
//   M5. run() copies scalar kv_settings (default_model, auto_approve,
//       max_agent_iterations, max_*_size) into the store.
//   M6. run() skips models with placeholder api_keys ("YOUR_API_KEY_HERE").
//   M7. run() ingests mcp/servers.json into per-server kv rows.
//   M8. run() renames migrated files to <name>.migrated when mark_files=true.
//   M9. run() does NOT rename files when mark_files=false (dry-run mode).
//  M10. run() is idempotent — calling it twice migrates nothing the
//       second time.

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include <daedalus/settings.h>
#include "core/SettingsMigrator.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// RAII temp HOME so we control where daedalus looks for config.json.
struct TmpHome {
    fs::path path;
    TmpHome(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus-migrator-" + tag + "-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path / ".config" / "daedalus" / "mcp");
        fs::create_directories(path / ".local" / "share" / "daedalus");
    }
    ~TmpHome() { std::error_code ec; fs::remove_all(path, ec); }

    fs::path config_root() const { return path / ".config" / "daedalus"; }
    fs::path config_json() const { return config_root() / "config.json"; }
    fs::path mcp_json()     const { return config_root() / "mcp" / "servers.json"; }
    fs::path data_root()    const { return path / ".local" / "share" / "daedalus"; }
    fs::path store_db()     const { return data_root() / "daedalus.db"; }
};

void write_config_json(const fs::path& p) {
    json j = {
        {"default_model", "alpha"},
        {"auto_approve", true},
        {"max_agent_iterations", 250},
        {"max_request_size", 5000000},
        {"max_response_size", 7000000},
        {"max_tool_result_size", 300000},
        {"models", {
            {"alpha", {
                {"host", "http://alpha.example"},
                {"api_key", "alpha-real-key"},
                {"model", "alpha-model"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
                {"max_context_size", 100000},
                {"temperature", 0.1}
            }},
            {"beta", {                         // placeholder api_key → skip
                {"host", "http://beta.example"},
                {"api_key", "YOUR_API_KEY_HERE"},
                {"model", "beta-model"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
                {"max_context_size", 100000}
            }},
            {"gamma", {
                {"host", "http://gamma.example"},
                {"api_key", "gamma-real-key"},
                {"model", "gamma-model"},
                {"endpoints", {{"chat", "/custom/path"}}},
                {"max_context_size", 100000},
                {"temperature", 0.9}
            }}
        }},
        {"serper-search", {
            {"api_key", "real-serper-key-12345"}
        }}
    };
    std::ofstream(p) << j.dump(2);
}

void write_mcp_json(const fs::path& p) {
    json j = {
        {"mcpServers", {
            {"server-a", {{"command", "echo"}, {"args", {"hi"}}}},
            {"server-b", {{"command", "cat"},  {"args", {"/etc/hostname"}}}}
        }}
    };
    std::ofstream(p) << j.dump(2);
}

int failures = 0;
#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                      \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond << "\n";       \
        ++failures;                                                     \
    }                                                                   \
} while (0)

// ===========================================================================
// M1: no legacy JSON → no migration needed
// ===========================================================================
void test_m1_no_legacy_no_migration() {
    TmpHome h("m1");
    // No config.json or mcp/servers.json written.
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    EXPECT(!daedalus::SettingsMigrator::needs_migration(
                store, h.config_json(), h.mcp_json()));
    std::cout << "[ok] M1: needs_migration=false when no legacy JSON\n";
}

// ===========================================================================
// M2: store already populated → no migration needed (idempotency)
// ===========================================================================
void test_m2_populated_store_no_migration() {
    TmpHome h("m2");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::ModelConfig m;
    m.name = "preset"; m.host = "x"; m.api_key = "k"; m.model_id = "p";
    store.upsert_model(m);
    EXPECT(!daedalus::SettingsMigrator::needs_migration(
                store, h.config_json(), h.mcp_json()));
    std::cout << "[ok] M2: needs_migration=false when store is populated\n";
}

// ===========================================================================
// M3: legacy JSON + empty store → migration needed
// ===========================================================================
void test_m3_legacy_json_needs_migration() {
    TmpHome h("m3");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    EXPECT(daedalus::SettingsMigrator::needs_migration(
                store, h.config_json(), h.mcp_json()));
    std::cout << "[ok] M3: needs_migration=true with legacy JSON + empty store\n";
}

// ===========================================================================
// M4: models migrated
// ===========================================================================
void test_m4_models_migrated() {
    TmpHome h("m4");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);

    auto a = store.get_model("alpha");
    EXPECT(a.has_value());
    if (a) {
        EXPECT(a->host  == "http://alpha.example");
        EXPECT(a->api_key == "alpha-real-key");
        EXPECT(a->temperature == 0.1);
    }
    auto g = store.get_model("gamma");
    EXPECT(g.has_value());
    if (g) {
        EXPECT(g->chat_endpoint == "/custom/path");
    }
    std::cout << "[ok] M4: models (alpha, gamma) migrated; beta skipped\n";
}

// ===========================================================================
// M5: scalar kv_settings migrated
// ===========================================================================
void test_m5_scalars_migrated() {
    TmpHome h("m5");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);

    EXPECT(store.get_string("default_model")        == "alpha");
    EXPECT(store.get_bool("auto_approve")           == true);
    EXPECT(store.get_int("max_agent_iterations")    == 250);
    EXPECT(store.get_int("max_request_size")        == 5000000);
    EXPECT(store.get_int("max_response_size")       == 7000000);
    EXPECT(store.get_int("max_tool_result_size")    == 300000);
    EXPECT(store.get_string("serper.api_key")       == "real-serper-key-12345");
    std::cout << "[ok] M5: scalars + serper key migrated\n";
}

// ===========================================================================
// M6: placeholder api_keys skipped
// ===========================================================================
void test_m6_placeholder_skipped() {
    TmpHome h("m6");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);
    EXPECT(!store.get_model("beta").has_value());
    EXPECT(store.get_model("alpha").has_value());
    std::cout << "[ok] M6: placeholder api_key (beta) skipped\n";
}

// ===========================================================================
// M7: mcp/servers.json ingested
// ===========================================================================
void test_m7_mcp_servers_migrated() {
    TmpHome h("m7");
    write_config_json(h.config_json());
    write_mcp_json(h.mcp_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);

    auto a = store.get_string("mcp.server.server-a", "");
    auto b = store.get_string("mcp.server.server-b", "");
    EXPECT(!a.empty());
    EXPECT(!b.empty());
    EXPECT(a.find("echo") != std::string::npos);
    EXPECT(b.find("cat")  != std::string::npos);
    std::cout << "[ok] M7: mcp/servers.json ingested\n";
}

// ===========================================================================
// M8: files renamed to .migrated when mark_files=true
// ===========================================================================
void test_m8_mark_files() {
    TmpHome h("m8");
    write_config_json(h.config_json());
    write_mcp_json(h.mcp_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/true);

    EXPECT(!fs::exists(h.config_json()));
    EXPECT(fs::exists(h.config_json().string() + ".migrated"));
    EXPECT(!fs::exists(h.mcp_json()));
    EXPECT(fs::exists(h.mcp_json().string() + ".migrated"));
    std::cout << "[ok] M8: migrated files renamed to *.migrated\n";
}

// ===========================================================================
// M9: dry-run mode (mark_files=false) leaves source files in place
// ===========================================================================
void test_m9_dry_run_preserves_source() {
    TmpHome h("m9");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);
    EXPECT(fs::exists(h.config_json()));
    EXPECT(!fs::exists(h.config_json().string() + ".migrated"));
    std::cout << "[ok] M9: dry-run (mark_files=false) leaves source in place\n";
}

// ===========================================================================
// M10: idempotent — second run is a no-op
// ===========================================================================
void test_m10_idempotent() {
    TmpHome h("m10");
    write_config_json(h.config_json());
    auto store = daedalus::SettingsStore::for_home(h.data_root());

    auto rep1 = daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);
    int count_first = static_cast<int>(store.list_models().size());

    auto rep2 = daedalus::SettingsMigrator::run(
        store, h.config_json(), h.mcp_json(), /*mark_files=*/false);
    EXPECT(rep2.skipped == true);
    EXPECT(static_cast<int>(store.list_models().size()) == count_first);
    std::cout << "[ok] M10: run() is idempotent\n";
}

}  // namespace

int main() {
    test_m1_no_legacy_no_migration();
    test_m2_populated_store_no_migration();
    test_m3_legacy_json_needs_migration();
    test_m4_models_migrated();
    test_m5_scalars_migrated();
    test_m6_placeholder_skipped();
    test_m7_mcp_servers_migrated();
    test_m8_mark_files();
    test_m9_dry_run_preserves_source();
    test_m10_idempotent();

    if (failures) {
        std::cerr << "\ntest_settings_migrator: " << failures
                  << " FAILED (of 10 tests)\n";
        return 1;
    }
    std::cout << "\ntest_settings_migrator: 10 tests passed\n";
    return 0;
}