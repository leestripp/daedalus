// §Phase 14: unit tests for the system_prompt resolution helper in
// demos/daedalus-cli/util.h.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>

#include "../demos/daedalus-cli/util.h"

namespace fs = std::filesystem;

namespace {
    // Write content to a file and return its path.
    fs::path write_tmp(const std::string& name, const std::string& content) {
        fs::path p = fs::temp_directory_path() / name;
        std::ofstream f(p);
        f << content;
        return p;
    }

    // Create a project dir with a per-project overlay.
    fs::path make_project_with_overlay(const std::string& overlay_content) {
        fs::path proj = fs::temp_directory_path() /
                        ("daedalus_test_proj_" +
                         std::to_string(std::rand()));
        fs::remove_all(proj);
        fs::create_directories(proj / ".daedalus");
        std::ofstream f(proj / ".daedalus" / "system_prompt.txt");
        f << overlay_content;
        return proj;
    }
}

TEST_SUITE("resolve_system_prompt") {

    TEST_CASE("CLI override takes priority over sidecar") {
        fs::path override = write_tmp("daedalus_override_test.txt",
                                      "OVERRIDE_PRIORITY");
        std::string p = resolve_system_prompt(override, "/tmp");
        CHECK(p == "OVERRIDE_PRIORITY");
        fs::remove(override);
    }

    TEST_CASE("CLI override missing → warning + fall through to sidecar") {
        fs::path bogus = "/tmp/daedalus_does_not_exist_file_xyz.txt";
        // Don't create the file. The function should warn and continue.
        std::string p = resolve_system_prompt(bogus, "/tmp");
        // Either empty (no sidecar nearby either) or contains the sidecar
        // text. We don't assert specific content — just that it didn't crash
        // and didn't include "OVERRIDE".
        CHECK(p.find("OVERRIDE") == std::string::npos);
    }

    TEST_CASE("Per-project overlay is appended to demo sidecar") {
        // We don't actually have control over the sidecar location in this
        // test (it's next to the executable, which is /tmp). But we CAN
        // test the overlay concatenation logic with a synthetic sidecar:
        // we just need to verify that, given a project with an overlay,
        // the overlay text appears in the result.
        fs::path proj = make_project_with_overlay("PROJECT_OVERLAY");
        std::string p = resolve_system_prompt({}, proj);
        CHECK(p.find("PROJECT_OVERLAY") != std::string::npos);
        CHECK(p.find("Per-project system prompt overlay") != std::string::npos);
        fs::remove_all(proj);
    }

    TEST_CASE("No sidecar, no override, no overlay → empty") {
        fs::path proj = fs::temp_directory_path() / "daedalus_empty_proj";
        fs::remove_all(proj);
        fs::create_directories(proj);
        std::string p = resolve_system_prompt({}, proj);
        // We didn't find a sidecar in this test (no binary next to it),
        // so result should be empty.
        CHECK(p.empty());
        fs::remove_all(proj);
    }
}