// SPDX-License-Identifier: BSL-1.0
// tests/test_migrate.cpp
//
// Unit tests for the --migrate-data module (src/migrate.cpp):
//   * detect_migrate_data_args() — argv detection (pure)
//   * compute_migration_plan()   — mapping table + existence probing
//   * apply_migration_plan()     — file/dir copying
//
// SAFETY: all filesystem work happens in unique temp dirs. XDG_CONFIG_HOME /
// XDG_DATA_HOME are redirected to temp dirs for the duration of each test so
// the real user state (~/.config/daedalus, ~/.daedalus) is never touched —
// `from` is always set explicitly, never the $HOME/.daedalus default.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "migrate.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace daedalus_cli;

namespace {

struct Argv {
    std::vector<std::string> storage{"icarus"};
    std::vector<char*>       ptrs;
    Argv(std::initializer_list<const char*> args) {
        for (auto* a : args) storage.emplace_back(a);
        for (auto& s : storage) ptrs.push_back(s.data());
    }
    int    argc() { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

// RAII env-var override (setenv/putenv restore).
struct EnvGuard {
    std::string name;
    bool        had_value = false;
    std::string old_value;
    EnvGuard(const char* n, const std::string& v) : name(n) {
        if (const char* cur = std::getenv(n)) { had_value = true; old_value = cur; }
        ::setenv(n, v.c_str(), 1);
    }
    ~EnvGuard() {
        if (had_value) ::setenv(name.c_str(), old_value.c_str(), 1);
        else           ::unsetenv(name.c_str());
    }
};

struct ScratchDir {
    fs::path dir;
    explicit ScratchDir(const std::string& tag) {
        dir = fs::temp_directory_path() /
              ("icarus_test_migrate_" + tag + "_" + std::to_string(::getpid()));
        fs::create_directories(dir);
    }
    ~ScratchDir() { std::error_code ec; fs::remove_all(dir, ec); }
    fs::path write(const fs::path& rel, const std::string& content) {
        fs::path p = dir / rel;
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << content;
        return p;
    }
};

// Point XDG at temp roots and build options with an explicit legacy root.
struct MigrateFixture {
    ScratchDir scratch;
    EnvGuard   xdg_config;
    EnvGuard   xdg_data;
    MigrateOptions opt;

    explicit MigrateFixture(const std::string& tag)
        : scratch(tag)
        , xdg_config("XDG_CONFIG_HOME", (scratch.dir / "xdg_config").string())
        , xdg_data("XDG_DATA_HOME", (scratch.dir / "xdg_data").string())
    {
        opt.from = scratch.dir / "legacy";   // NEVER the real ~/.daedalus
    }

    fs::path write(const fs::path& rel, const std::string& content) {
        return scratch.write(rel, content);
    }
};

const MigratePlan::Entry* find_entry(const MigratePlan& plan,
                                     const std::string& from_suffix) {
    for (const auto& e : plan.entries)
        if (e.from.string().ends_with(from_suffix)) return &e;
    return nullptr;
}

} // namespace

TEST_CASE("migrate: detect_migrate_data_args — absent") {
    Argv a{"--auto", "--no-color"};
    auto r = detect_migrate_data_args(a.argc(), a.argv());
    CHECK(r.present == false);
    CHECK(r.consumed_args.empty());
}

TEST_CASE("migrate: detect_migrate_data_args — --migrate-data is a dry run") {
    Argv a{"--migrate-data"};
    auto r = detect_migrate_data_args(a.argc(), a.argv());
    CHECK(r.present == true);
    CHECK(r.options.dry_run == true);
}

TEST_CASE("migrate: detect_migrate_data_args — --migrate-data-apply disables dry run") {
    Argv a{"--migrate-data-apply"};
    auto r = detect_migrate_data_args(a.argc(), a.argv());
    CHECK(r.present == true);
    CHECK(r.options.dry_run == false);
}

TEST_CASE("migrate: detect_migrate_data_args — --migrate-data-from captures DIR") {
    Argv a{"--migrate-data-from", "/tmp/legacy-root"};
    auto r = detect_migrate_data_args(a.argc(), a.argv());
    CHECK(r.present == true);
    CHECK(r.options.from == fs::path("/tmp/legacy-root"));
    CHECK(r.consumed_args.size() == 2);
}

TEST_CASE("migrate: detect_migrate_data_args — --migrate-data-verbose alone doesn't trigger") {
    Argv a{"--migrate-data-verbose"};
    auto r = detect_migrate_data_args(a.argc(), a.argv());
    CHECK(r.present == false);          // verbose only modifies, never triggers
    CHECK(r.options.verbose == true);
}

TEST_CASE("migrate: compute_migration_plan — four known mappings, all absent") {
    MigrateFixture f("plan_absent");
    auto plan = compute_migration_plan(f.opt);

    CHECK(plan.from_root == f.opt.from);
    REQUIRE(plan.entries.size() == 4);   // settings.json, servers.json, .env, skills/
    for (const auto& e : plan.entries)
        CHECK(e.from_exists == false);
}

TEST_CASE("migrate: compute_migration_plan — existing file reported with size") {
    MigrateFixture f("plan_file");
    f.write("legacy/settings/settings.json", "{\"k\":1}");

    auto plan = compute_migration_plan(f.opt);
    const auto* e = find_entry(plan, "settings/settings.json");
    REQUIRE(e != nullptr);
    CHECK(e->from_exists == true);
    CHECK(e->size_bytes == 7);
    CHECK(e->to == plan.xdg_config_home / "config.json");
}

TEST_CASE("migrate: compute_migration_plan — skills dir size is summed recursively") {
    MigrateFixture f("plan_dir");
    f.write("legacy/skills/pack-a/SKILL.md", "12345");
    f.write("legacy/skills/pack-a/nested/x.txt", "abc");

    auto plan = compute_migration_plan(f.opt);
    const auto* e = find_entry(plan, "skills");
    REQUIRE(e != nullptr);
    CHECK(e->from_exists == true);
    CHECK(e->size_bytes == 8);           // 5 + 3 bytes
    CHECK(e->to == plan.xdg_data_home / "skills" / "installed");
}

TEST_CASE("migrate: apply_migration_plan copies files to XDG destinations") {
    MigrateFixture f("apply");
    f.write("legacy/settings/settings.json", "{\"theme\":\"dark\"}");
    f.write("legacy/mcp/servers.json", "[]");

    auto plan = compute_migration_plan(f.opt);
    int copied = apply_migration_plan(plan, /*verbose=*/false);

    CHECK(copied == 2);
    // destinations live under the redirected XDG temp roots
    std::ifstream cfg(plan.xdg_config_home / "config.json");
    std::string content((std::istreambuf_iterator<char>(cfg)),
                         std::istreambuf_iterator<char>());
    CHECK(content == "{\"theme\":\"dark\"}");
    CHECK(fs::exists(plan.xdg_config_home / "mcp" / "servers.json"));
    // legacy source untouched (copy, never move)
    CHECK(fs::exists(f.opt.from / "settings" / "settings.json"));
}

TEST_CASE("migrate: apply_migration_plan copies skills dir recursively") {
    MigrateFixture f("apply_dir");
    f.write("legacy/skills/cmake-helper/SKILL.md", "# cmake\n");

    auto plan = compute_migration_plan(f.opt);
    int copied = apply_migration_plan(plan, false);

    CHECK(copied == 1);
    CHECK(fs::exists(plan.xdg_data_home / "skills" / "installed" /
                     "cmake-helper" / "SKILL.md"));
}

TEST_CASE("migrate: apply_migration_plan on empty legacy root copies nothing") {
    MigrateFixture f("apply_empty");
    auto plan = compute_migration_plan(f.opt);
    CHECK(apply_migration_plan(plan, false) == 0);
}
