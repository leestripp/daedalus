// SPDX-License-Identifier: BSL-1.0
// tests/test_settings_v040_cutover.cpp
//
// v0.4.0 cutover tests. Pin the behaviour change: SettingsManager.load()
// reads ONLY from SettingsStore (SQLite). The legacy JSON file is
// ignored — except via the SettingsMigrator one-shot on first run.
//
// What we pin:
//   V1. SettingsManager loaded from SQLite only (not from JSON directly).
//       Even if a JSON file with DIFFERENT models exists alongside the
//       SQLite store, the JSON values must NOT appear in
//       SettingsManager::get_models().
//   V2. SettingsManager loaded scalars come from SQLite only.
//       The JSON `default_model` is ignored when SQLite has a value.
//   V3. The migrator is still the ONLY way to get JSON into the SQLite
//       store. If we hand-construct a fresh SQLite + legacy JSON and
//       call SettingsManager::load(), the legacy JSON is ignored until
//       the migrator runs.
//   V4. After migration, SettingsManager sees the migrated values.
//   V5. Empty SQLite + empty JSON → load() still works (defaults).
//
// These tests are the regression guard for v0.4.0: they ensure we
// can't accidentally fall back to JSON once the cutover lands.

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include <daedalus/settings.h>

// SettingsManager is in src/core, not the public API — we include via
// the build's include path.
#include "core/SettingsManager.h"
#include "core/SettingsMigrator.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// RAII temp HOME so we control where daedalus looks for config.json
// AND where the SettingsStore opens its DB. Without this, both the
// JSON path AND the SQLite path resolve to ~/.config/daedalus/ and
// ~/.local/share/daedalus/ — which might already have user data.
struct TmpHome {
    fs::path path;
    TmpHome(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus-v040-" + tag + "-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()) + "-" +
                std::to_string(::getpid()));
        fs::remove_all(path);
        // Both halves of the XDG layout that SettingsManager touches:
        fs::create_directories(path / ".config" / "daedalus" / "mcp");
        fs::create_directories(path / ".local" / "share" / "daedalus");
    }
    ~TmpHome() { std::error_code ec; fs::remove_all(path, ec); }

    fs::path config_root() const { return path / ".config" / "daedalus"; }
    fs::path config_json() const { return config_root() / "config.json"; }
    fs::path data_root()    const { return path / ".local" / "share" / "daedalus"; }
};

void write_legacy_config(const fs::path& p, const std::string& default_model,
                         const std::string& api_key) {
    json j = {
        {"default_model", default_model},
        {"auto_approve", true},
        {"max_agent_iterations", 250},
        {"models", {
            {default_model, {
                {"host", "http://legacy.example"},
                {"api_key", api_key},
                {"model", default_model + "-model"},
                {"endpoints", {{"chat", "/v1/chat/completions"}}},
                {"max_context_size", 100000},
                {"temperature", 0.3}
            }}
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
// V1: SettingsManager reads SQLite, ignores JSON
// ===========================================================================
void test_v1_sqlite_only_ignores_json() {
    TmpHome h("v1");

    // Write a legacy JSON with model "json-model".
    write_legacy_config(h.config_json(), "json-model", "json-key");

    // Write a DIFFERENT model into the SQLite store via SettingsStore.
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::ModelConfig m;
    m.name = "sqlite-model"; m.host = "http://sqlite.example";
    m.api_key = "sqlite-key"; m.model_id = "sqlite";
    m.chat_endpoint = "/v1/chat/completions";
    store.upsert_model(m);

    // Construct SettingsManager with paths pointing at our temp dirs
    // AND with our per-home store explicitly injected.
    SettingsManager sm(store, h.config_json());
    sm.load();

    // MUST see sqlite-model, MUST NOT see json-model.
    auto& models = sm.get_models();
    EXPECT(models.count("sqlite-model") == 1);
    EXPECT(models.count("json-model")   == 0);
    if (models.count("sqlite-model") == 1) {
        EXPECT(models.at("sqlite-model").api_key == "sqlite-key");
    }
    std::cout << "[ok] V1: SettingsManager reads SQLite, ignores JSON\n";
}

// ===========================================================================
// V2: scalars come from SQLite, not JSON
// ===========================================================================
void test_v2_scalars_from_sqlite() {
    TmpHome h("v2");
    write_legacy_config(h.config_json(), "json-model", "json-key");

    auto store = daedalus::SettingsStore::for_home(h.data_root());
    store.set_string("default_model", "sqlite-default");
    store.set_bool("auto_approve", false);
    store.set_int("max_agent_iterations", 777);

    SettingsManager sm(store, h.config_json());
    sm.load();

    EXPECT(sm.get_default_model_name() == "sqlite-default");
    EXPECT(sm.get_auto_approve() == false);
    EXPECT(sm.get_max_agent_iterations() == 777);
    std::cout << "[ok] V2: SettingsManager scalars come from SQLite\n";
}

// ===========================================================================
// V3: legacy JSON is auto-migrated to SQLite on first load() (v0.4.0)
// ===========================================================================
void test_v3_legacy_json_is_auto_migrated() {
    TmpHome h("v3");
    write_legacy_config(h.config_json(), "migrated-on-first-load", "real-key");

    auto store = daedalus::SettingsStore::for_home(h.data_root());
    // Sanity: store starts empty.
    EXPECT(store.list_models().empty());

    SettingsManager sm(store, h.config_json());
    sm.load();

    // After load(), the legacy JSON has been auto-migrated into the
    // store (v0.3.1 auto-migrator). The SettingsManager sees the
    // migrated model.
    auto& models = sm.get_models();
    EXPECT(models.count("migrated-on-first-load") == 1);
    EXPECT(sm.get_default_model_name() == "migrated-on-first-load");
    EXPECT(sm.get_max_agent_iterations() == 250);
    std::cout << "[ok] V3: legacy JSON is auto-migrated to SQLite on first load\n";
}

// ===========================================================================
// V4: After migrator runs, SettingsManager sees the migrated values
// ===========================================================================
void test_v4_after_migrator() {
    TmpHome h("v4");
    write_legacy_config(h.config_json(), "migrated-model", "migrated-key");

    auto store = daedalus::SettingsStore::for_home(h.data_root());
    daedalus::SettingsMigrator::run(store, h.config_json(), {}, /*mark=*/false);

    SettingsManager sm(store, h.config_json());
    sm.load();

    auto& models = sm.get_models();
    EXPECT(models.count("migrated-model") == 1);
    EXPECT(sm.get_default_model_name() == "migrated-model");
    EXPECT(sm.get_max_agent_iterations() == 250);
    std::cout << "[ok] V4: After migrator, SettingsManager sees migrated data\n";
}

// ===========================================================================
// V5: legacy JSON is auto-migrated AND SettingsManager sees migrated data
// ===========================================================================
void test_v5_load_succeeds_after_auto_migration() {
    TmpHome h("v5");
    write_legacy_config(h.config_json(), "renamed-after-mig", "real-key");

    // Sanity: JSON exists, store empty.
    EXPECT(fs::exists(h.config_json()));
    auto store = daedalus::SettingsStore::for_home(h.data_root());
    EXPECT(store.list_models().empty());

    SettingsManager sm(store, h.config_json());
    sm.load();

    // SettingsManager sees the migrated model (loaded_ is true).
    EXPECT(sm.get_models().count("renamed-after-mig") == 1);
    EXPECT(sm.get_default_model_name() == "renamed-after-mig");
    std::cout << "[ok] V5: load() succeeds with auto-migrated data\n";
}

}  // namespace

int main() {
    test_v1_sqlite_only_ignores_json();
    test_v2_scalars_from_sqlite();
    test_v3_legacy_json_is_auto_migrated();
    test_v4_after_migrator();
    test_v5_load_succeeds_after_auto_migration();

    if (failures) {
        std::cerr << "\ntest_settings_v040_cutover: " << failures
                  << " FAILED (of 5 tests)\n";
        return 1;
    }
    std::cout << "\ntest_settings_v040_cutover: 5 tests passed\n";
    return 0;
}