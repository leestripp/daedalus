// SPDX-License-Identifier: BSL-1.0
// tests/test_system_prompt.cpp
//
// Unit tests for resolve_system_prompt() in src/util.h.
// Uses real temp directories under fs::temp_directory_path() — no shared
// state, cleaned up at the end of each test case.
//
// Priority chain under test:
//   1. --system-prompt FILE override (highest)
//   2. <project_root>/AGENTS.md
// Quirk documented by the implementation: if an override file is given but
// does NOT exist, AGENTS.md is NOT consulted (the override path being
// non-empty short-circuits the fallback) and the result is empty.

#include <doctest/doctest.h>

#include "util.h"

#include <fstream>

namespace {

// Unique scratch dir per test case, removed on destruction.
struct ScratchDir {
    fs::path dir;
    explicit ScratchDir(const std::string& tag) {
        dir = fs::temp_directory_path() /
              ("icarus_test_sysprompt_" + tag + "_" +
               std::to_string(::getpid()));
        fs::create_directories(dir);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path write(const std::string& name, const std::string& content) {
        fs::path p = dir / name;
        std::ofstream f(p);
        f << content;
        return p;
    }
};

} // namespace

TEST_CASE("system_prompt: empty when no source exists") {
    ScratchDir s("empty");
    std::string out = resolve_system_prompt({}, s.dir);
    CHECK(out.empty());
}

TEST_CASE("system_prompt: reads <project_root>/AGENTS.md") {
    ScratchDir s("agentsmd");
    s.write("AGENTS.md", "# Project rules\nDo the thing.\n");

    std::string out = resolve_system_prompt({}, s.dir);
    CHECK(out == "# Project rules\nDo the thing.\n");
}

TEST_CASE("system_prompt: override file wins over AGENTS.md") {
    ScratchDir s("priority");
    s.write("AGENTS.md", "project prompt");
    fs::path override_file = s.write("custom.md", "override prompt");

    std::string out = resolve_system_prompt(override_file, s.dir);
    CHECK(out == "override prompt");
}

TEST_CASE("system_prompt: override used even when no project_root given") {
    ScratchDir s("only");
    fs::path override_file = s.write("only.md", "just the override");

    std::string out = resolve_system_prompt(override_file, {});
    CHECK(out == "just the override");
}

TEST_CASE("system_prompt: missing override short-circuits AGENTS.md fallback") {
    ScratchDir s("quirk");
    s.write("AGENTS.md", "project prompt");
    fs::path missing = s.dir / "does-not-exist.md";

    // Documented behaviour: override path is non-empty, so the AGENTS.md
    // branch is skipped even though the override file doesn't exist.
    std::string out = resolve_system_prompt(missing, s.dir);
    CHECK(out.empty());
}

TEST_CASE("system_prompt: empty project_root with no override yields empty") {
    std::string out = resolve_system_prompt({}, {});
    CHECK(out.empty());
}
