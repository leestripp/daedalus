// SPDX-License-Identifier: BSL-1.0
// tests/test_searxng_settings.cpp
//
// §SEARXNG-CORE (2026-09-21) regression/feature test: SearXNG metasearch
// instance config (SettingsStore searxng_config table, schema v7) +
// core/SearxngConfig validation helpers.
//
// Coverage:
//   1. Fresh store returns defaults (empty base_url, enabled=true).
//   2. set/get round-trip (base_url, username, password, enabled).
//   3. Upsert: a second set_searxng_settings() updates the singleton row.
//   4. enabled=false persists.
//   5. looks_like_valid_searxng_url accept/reject matrix.
//   6. normalize_searxng_base_url strips trailing slashes.
//   7. v6→v7 migration: a v6-era DB gains searxng_config on open.
//
// SearxngSearchTool network behaviour is intentionally NOT covered here:
// it requires a live SearXNG instance. The tool degrades to a descriptive
// error when unconfigured (same contract as SerperSearchTool).
//
// Hermetic — temp dirs only, no network, no spawns.

#include <daedalus/settings.h>
#include "core/SearxngConfig.h"

#include <cassert>
#include <filesystem>
#include <iostream>

#include <db/Database.h>      // OmniNet::DB::Database — build a v6-era DB

namespace fs = std::filesystem;

namespace {

fs::path fresh_dir(const std::string& tag) {
    fs::path d = fs::temp_directory_path() / ("searxng-test-" + tag);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

}  // namespace

// 1: fresh store → defaults.
void test_defaults() {
    auto root = fresh_dir("defaults");
    auto store = daedalus::SettingsStore::for_home(root / "home");
    const auto s = store.get_searxng_settings();
    assert(s.base_url.empty());
    assert(s.username.empty());
    assert(s.password.empty());
    assert(s.enabled);
    fs::remove_all(root);
    std::cout << "[ok] test_defaults" << std::endl;
}

// 2: set/get round-trip.
void test_roundtrip() {
    auto root = fresh_dir("roundtrip");
    auto store = daedalus::SettingsStore::for_home(root / "home");

    daedalus::SearxngSettings in;
    in.base_url = "https://leestrippai.com:8443";
    in.username = "agent";
    in.password = "s3cret!";
    in.enabled  = true;
    store.set_searxng_settings(in);

    const auto out = store.get_searxng_settings();
    assert(out.base_url == in.base_url);
    assert(out.username == in.username);
    assert(out.password == in.password);
    assert(out.enabled == in.enabled);

    fs::remove_all(root);
    std::cout << "[ok] test_roundtrip" << std::endl;
}

// 3: upsert — a second set updates the singleton row in place.
void test_upsert() {
    auto root = fresh_dir("upsert");
    auto store = daedalus::SettingsStore::for_home(root / "home");

    daedalus::SearxngSettings a;
    a.base_url = "https://old.example.com";
    store.set_searxng_settings(a);

    daedalus::SearxngSettings b;
    b.base_url = "https://new.example.com";
    b.username = "u2";
    b.password = "p2";
    store.set_searxng_settings(b);

    const auto out = store.get_searxng_settings();
    assert(out.base_url == "https://new.example.com");
    assert(out.username == "u2");
    assert(out.password == "p2");
    assert(out.enabled);   // b.enabled defaulted true

    fs::remove_all(root);
    std::cout << "[ok] test_upsert" << std::endl;
}

// 4: enabled=false persists.
void test_disabled_persists() {
    auto root = fresh_dir("disabled");
    auto store = daedalus::SettingsStore::for_home(root / "home");

    daedalus::SearxngSettings in;
    in.base_url = "https://search.example.com";
    in.enabled  = false;
    store.set_searxng_settings(in);

    const auto out = store.get_searxng_settings();
    assert(out.base_url == "https://search.example.com");
    assert(!out.enabled);

    fs::remove_all(root);
    std::cout << "[ok] test_disabled_persists" << std::endl;
}

// 5: URL validation matrix.
void test_url_validation() {
    using daedalus::looks_like_valid_searxng_url;
    assert(looks_like_valid_searxng_url("https://leestrippai.com:8443"));
    assert(looks_like_valid_searxng_url("http://127.0.0.1:8888"));
    assert(looks_like_valid_searxng_url("https://search.example.com/searxng"));

    assert(!looks_like_valid_searxng_url(""));
    assert(!looks_like_valid_searxng_url("ftp://example.com"));
    assert(!looks_like_valid_searxng_url("example.com"));
    assert(!looks_like_valid_searxng_url("https://"));        // scheme only
    assert(!looks_like_valid_searxng_url("http://"));         // scheme only
    assert(!looks_like_valid_searxng_url("https://exa mple.com"));
    assert(!looks_like_valid_searxng_url("https://example.com/\"q\""));

    std::cout << "[ok] test_url_validation" << std::endl;
}

// 6: trailing-slash normalization.
void test_normalize() {
    using daedalus::normalize_searxng_base_url;
    assert(normalize_searxng_base_url("https://h.example.com") == "https://h.example.com");
    assert(normalize_searxng_base_url("https://h.example.com/") == "https://h.example.com");
    assert(normalize_searxng_base_url("https://h.example.com///") == "https://h.example.com");
    assert(normalize_searxng_base_url("") == "");
    std::cout << "[ok] test_normalize" << std::endl;
}

// 7: a v6-era DB (schema_version=6, no searxng_config) gains the table
//    via migration when the store opens it.
void test_migration_from_v6() {
    auto root = fresh_dir("migrate");
    const fs::path home = root / "home";
    fs::create_directories(home);
    const fs::path db_path = home / "daedalus.db";

    {
        // Build a minimal v6-era DB by hand.
        OmniNet::DB::Database db;
        db.open(db_path.string());
        db.execute("CREATE TABLE schema_version (version INTEGER PRIMARY KEY)");
        db.execute("INSERT INTO schema_version(version) VALUES (6)");
        db.execute("CREATE TABLE serper_config ("
                   " id INTEGER PRIMARY KEY CHECK (id = 1),"
                   " api_key TEXT NOT NULL)");
    }

    {
        auto store = daedalus::SettingsStore::for_home(home);
        // The v7 migration must have run: the accessor round-trips.
        daedalus::SearxngSettings in;
        in.base_url = "https://migrated.example.com";
        in.username = "mig";
        store.set_searxng_settings(in);
        const auto out = store.get_searxng_settings();
        assert(out.base_url == "https://migrated.example.com");
        assert(out.username == "mig");
    }

    fs::remove_all(root);
    std::cout << "[ok] test_migration_from_v6" << std::endl;
}

int main() {
    test_defaults();
    test_roundtrip();
    test_upsert();
    test_disabled_persists();
    test_url_validation();
    test_normalize();
    test_migration_from_v6();
    std::cout << "ALL SEARXNG SETTINGS TESTS PASSED" << std::endl;
    return 0;
}
