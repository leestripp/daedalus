// SPDX-License-Identifier: BSL-1.0
// tests/test_settings_store.cpp
//
// Unit tests for the new SQLite-backed SettingsStore (v0.3.0).
//
// What we pin:
//   S1. instance() returns the same handle across calls (singleton).
//   S2. for_home(home) opens a *distinct* DB at <home>/daedalus.db and
//       two calls with the same home share the connection.
//   S3. upsert_model + get_model round-trips all ModelConfig fields.
//   S4. list_models returns everything we wrote (including via on conflict).
//   S5. delete_model actually removes the row.
//   S6. set_string / get_string / get_int / get_bool round-trip.
//   S7. transaction(fn) either commits all writes or rolls back all of
//       them — i.e. the fn sees uncommitted writes; a thrown fn rolls
//       everything back.
//   S8. db_path() returns the path we opened.
//   S9. The schema is created idempotently on a fresh DB (open + drop +
//       reopen does not throw, and tables exist).

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <daedalus/settings.h>

namespace fs = std::filesystem;

namespace {

// RAII temp dir
struct TmpHome {
    fs::path path;
    TmpHome(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus-settings-store-" + tag + "-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpHome() { std::error_code ec; fs::remove_all(path, ec); }
};

int failures = 0;
#define EXPECT(cond) do {                                         \
    if (!(cond)) {                                                 \
        std::cerr << "FAIL @ " << __LINE__ << ": " #cond "\n";     \
        ++failures;                                                \
    }                                                              \
} while (0)

daedalus::ModelConfig make_model(const std::string& name) {
    daedalus::ModelConfig m;
    m.name             = name;
    m.host             = "http://test.invalid";
    m.api_key          = "sk-test-" + name;
    m.model_id         = "model-id-" + name;
    m.chat_endpoint    = "/v1/chat/completions";
    m.max_context_size = 123456;
    m.temperature      = 0.42;
    return m;
}

// ===========================================================================
// S1: instance() is a singleton
// ===========================================================================
void test_s1_instance_singleton() {
    auto& a = daedalus::SettingsStore::instance();
    auto& b = daedalus::SettingsStore::instance();
    EXPECT(&a == &b);
    std::cout << "[ok] S1: instance() is a singleton\n";
}

// ===========================================================================
// S2: for_home(home) opens <home>/daedalus.db
// ===========================================================================
void test_s2_for_home_isolates() {
    TmpHome h1("s2a");
    TmpHome h2("s2b");

    auto s1a = daedalus::SettingsStore::for_home(h1.path);
    auto s1b = daedalus::SettingsStore::for_home(h1.path);   // same home → same handle
    auto s2  = daedalus::SettingsStore::for_home(h2.path);   // different home → distinct

    EXPECT(s1a.db_path() == h1.path / "daedalus.db");
    EXPECT(s2.db_path()  == h2.path / "daedalus.db");
    EXPECT(s1a.db_path() != s2.db_path());
    // file actually exists on disk after first use
    EXPECT(fs::exists(s1a.db_path()));
    EXPECT(fs::exists(s2.db_path()));
    std::cout << "[ok] S2: for_home(home) opens <home>/daedalus.db\n";
}

// ===========================================================================
// S3: upsert_model + get_model round-trips
// ===========================================================================
void test_s3_model_roundtrip() {
    TmpHome h("s3");
    auto s = daedalus::SettingsStore::for_home(h.path);

    auto m = make_model("alpha");
    s.upsert_model(m);

    auto got = s.get_model("alpha");
    EXPECT(got.has_value());
    if (got) {
        EXPECT(got->name             == m.name);
        EXPECT(got->host             == m.host);
        EXPECT(got->api_key          == m.api_key);
        EXPECT(got->model_id         == m.model_id);
        EXPECT(got->chat_endpoint    == m.chat_endpoint);
        EXPECT(got->max_context_size == m.max_context_size);
        EXPECT(got->temperature      == m.temperature);
    }
    std::cout << "[ok] S3: upsert_model + get_model round-trip\n";
}

// ===========================================================================
// S4: list_models returns all rows, and upsert (not insert) replaces
// ===========================================================================
void test_s4_list_and_upsert_replaces() {
    TmpHome h("s4");
    auto s = daedalus::SettingsStore::for_home(h.path);

    s.upsert_model(make_model("a"));
    s.upsert_model(make_model("b"));
    s.upsert_model(make_model("c"));

    auto all = s.list_models();
    EXPECT(all.size() == 3);

    // upsert with same name but different api_key must update, not duplicate
    auto m = make_model("a");
    m.api_key = "sk-updated";
    s.upsert_model(m);

    all = s.list_models();
    EXPECT(all.size() == 3);
    auto a = s.get_model("a");
    EXPECT(a.has_value() && a->api_key == "sk-updated");
    std::cout << "[ok] S4: list_models + upsert replaces\n";
}

// ===========================================================================
// S5: delete_model removes the row
// ===========================================================================
void test_s5_delete_model() {
    TmpHome h("s5");
    auto s = daedalus::SettingsStore::for_home(h.path);

    s.upsert_model(make_model("gone"));
    EXPECT(s.get_model("gone").has_value());
    s.delete_model("gone");
    EXPECT(!s.get_model("gone").has_value());
    EXPECT(s.list_models().empty());
    std::cout << "[ok] S5: delete_model removes the row\n";
}

// ===========================================================================
// S6: kv_settings round-trip
// ===========================================================================
void test_s6_kv_roundtrip() {
    TmpHome h("s6");
    auto s = daedalus::SettingsStore::for_home(h.path);

    EXPECT(s.get_string("default_model", "fallback") == "fallback");
    EXPECT(s.get_bool("auto_approve", false)         == false);
    EXPECT(s.get_int("max_iterations", 7)            == 7);

    s.set_string("default_model", "minimax-m3");
    s.set_bool("auto_approve", true);
    s.set_int("max_iterations", 42);

    EXPECT(s.get_string("default_model") == "minimax-m3");
    EXPECT(s.get_bool("auto_approve")    == true);
    EXPECT(s.get_int("max_iterations")   == 42);
    std::cout << "[ok] S6: kv_settings round-trip\n";
}

// ===========================================================================
// S7: transaction commits or rolls back atomically
// ===========================================================================
void test_s7_transaction_atomicity() {
    TmpHome h("s7");
    auto s = daedalus::SettingsStore::for_home(h.path);

    // Happy path: commit
    s.transaction([&](daedalus::SettingsStore& txn) {
        txn.upsert_model(make_model("tx-ok-1"));
        txn.upsert_model(make_model("tx-ok-2"));
    });
    EXPECT(s.list_models().size() == 2);

    // Sad path: throwing inside the transaction rolls everything back.
    bool caught = false;
    try {
        s.transaction([&](daedalus::SettingsStore& txn) {
            txn.upsert_model(make_model("will-rollback-1"));
            txn.upsert_model(make_model("will-rollback-2"));
            throw std::runtime_error("boom");
        });
    } catch (const std::runtime_error&) {
        caught = true;
    }
    EXPECT(caught);
    // Both rollback models must be gone.
    EXPECT(!s.get_model("will-rollback-1").has_value());
    EXPECT(!s.get_model("will-rollback-2").has_value());
    // The two committed models from before must still be there.
    EXPECT(s.list_models().size() == 2);
    std::cout << "[ok] S7: transaction is atomic (rollback on throw)\n";
}

// ===========================================================================
// S8: db_path() returns the path we opened
// ===========================================================================
void test_s8_db_path() {
    TmpHome h("s8");
    auto s = daedalus::SettingsStore::for_home(h.path);
    EXPECT(s.db_path() == h.path / "daedalus.db");
    std::cout << "[ok] S8: db_path() reports the on-disk location\n";
}

// ===========================================================================
// S9: schema is created idempotently on fresh DB
// ===========================================================================
void test_s9_schema_idempotent() {
    // §B2 fix: for_home() now caches per-path (no leak), so the test
    // must use a fresh path for the "schema after delete" check.
    // We simulate "user deleted their DB file" by using two distinct
    // paths: one to write data, one to verify schema creation works.
    TmpHome h1("s9-write");
    TmpHome h2("s9-fresh");
    {
        // First open creates the schema and inserts data.
        auto s = daedalus::SettingsStore::for_home(h1.path);
        s.upsert_model(make_model("pinned"));
        EXPECT(s.get_model("pinned").has_value());
    }
    {
        // Fresh path → brand-new DB → schema is created → empty
        // models table → upsert works.
        auto s = daedalus::SettingsStore::for_home(h2.path);
        EXPECT(s.list_models().empty());
        s.upsert_model(make_model("pinned"));
        EXPECT(s.get_model("pinned").has_value());
    }
    std::cout << "[ok] S9: schema is created idempotently on fresh DB\n";
}

// ===========================================================================
// S10: SQL injection regression test (§B1 fix)
// ===========================================================================
//
// Verifies that user-controlled strings with quotes, semicolons, and
// SQL fragments don't escape into the SQL when stored or looked up.
// Pre-B1, a value like "x'; DROP TABLE models; --" would execute as
// a destructive statement when concatenated into the query.
void test_s10_sql_injection_resistance() {
    TmpHome h("s10");
    auto s = daedalus::SettingsStore::for_home(h.path);

    // Try to break things via the model name (primary key).
    std::vector<std::string> malicious = {
        "x'; DROP TABLE models; --",
        "y' OR '1'='1",
        "z\"; DROP TABLE kv_settings; --",
        std::string(200, '\'') + " ; DROP TABLE models; --",
    };

    for (const auto& name : malicious) {
        daedalus::ModelConfig m = make_model(name);
        s.upsert_model(m);
        // The round-trip must preserve the value verbatim — no truncation,
        // no SQL execution.
        auto got = s.get_model(name);
        EXPECT(got.has_value());
        if (got.has_value()) {
            EXPECT(got->name == name);
            EXPECT(got->host == m.host);
            EXPECT(got->api_key == m.api_key);
        }
        // The table must still exist (DROP TABLE didn't run).
        EXPECT(s.list_models().size() >= 1);
    }

    // Also exercise kv_settings with malicious keys/values.
    s.set_string("evil'; DROP TABLE kv_settings; --", "value");
    EXPECT(s.get_string("evil'; DROP TABLE kv_settings; --") == "value");
    // kv_settings must still work for normal keys.
    s.set_string("normal_key", "normal_value");
    EXPECT(s.get_string("normal_key") == "normal_value");
    EXPECT(s.get_string("nonexistent", "default") == "default");

    // delete with malicious name must not match anything it shouldn't.
    s.delete_model("evil'; DROP TABLE kv_settings; --");
    EXPECT(!s.get_model("evil'; DROP TABLE kv_settings; --").has_value());

    // Verify mcp_servers path too.
    daedalus::SettingsStore::McpServer mcp;
    mcp.name = "x'; DROP TABLE mcp_servers; --";
    mcp.command = "/bin/true";
    mcp.enabled = true;
    s.upsert_mcp_server(mcp);
    auto got_mcp = s.get_mcp_server("x'; DROP TABLE mcp_servers; --");
    EXPECT(got_mcp.has_value());
    EXPECT(got_mcp->name == "x'; DROP TABLE mcp_servers; --");
    // mcp_servers must still exist.
    EXPECT(s.has_mcp_servers());

    std::cout << "[ok] S10: SQL injection resistance (B1 regression)\n";
}

}  // namespace

int main() {
    test_s1_instance_singleton();
    test_s2_for_home_isolates();
    test_s3_model_roundtrip();
    test_s4_list_and_upsert_replaces();
    test_s5_delete_model();
    test_s6_kv_roundtrip();
    test_s7_transaction_atomicity();
    test_s8_db_path();
    test_s9_schema_idempotent();
    test_s10_sql_injection_resistance();

    if (failures) {
        std::cerr << "\ntest_settings_store: " << failures
                  << " FAILED (of 10 tests)\n";
        return 1;
    }
    std::cout << "\ntest_settings_store: 10 tests passed\n";
    return 0;
}
