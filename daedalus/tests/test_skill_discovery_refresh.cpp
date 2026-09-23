// ============================================================================
// tests/test_skill_discovery_refresh.cpp
//
// §SKILL-RESCAN regression test (2026-09-17 bug report, MonkeyByte/HiveMind):
// discover_skills served the session-start cache forever, so the documented
// "create a skill, call discover_skills again to pick it up" workflow could
// never work; the `directory` argument was silently ignored; and the
// project-local `.agents/skills/` default was resolved against the daemon
// process CWD instead of AgentConfig.project_root, so GUI hosts never saw
// project-local skills.
//
// Fix: init_after_settings() takes an optional project_root anchor;
// AgentImpl passes cfg_.project_root; DiscoverSkillsTool honours
// `directory` (fresh scan, cache bypass) and refresh_cache()es otherwise.
//
// Hermetic — temp dirs only, no network, no live LLM calls.
// ============================================================================
#include "core/SkillManager.h"
#include "tools/SkillTool.h"
#include "core/FileIO.h"

#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;

static void make_skill(const fs::path& dir, const std::string& name) {
    fs::create_directories(dir);
    std::ofstream f(dir / "SKILL.md");
    f << "---\nname: " << name << "\ndescription: " << name
      << " skill\nversion: 1.0.0\n---\n\nbody\n";
}

int main() {
    const fs::path root = fs::temp_directory_path() / "skill-fix-proof";
    fs::remove_all(root);
    const fs::path project_root = root / "project";
    fs::create_directories(project_root);

    // --- C: project_root anchoring -------------------------------------
    SkillManager sm;
    sm.init_after_settings(project_root);
    bool anchored = false;
    for (const auto& d : sm.skill_directories()) {
        if (fs::weakly_canonical(d) ==
            fs::weakly_canonical(project_root / ".agents" / "skills")) {
            anchored = true;
        }
        assert(!d.is_relative() || d == fs::path(".agents/skills/"));
    }
    assert(anchored && "project-local dir must be anchored at project_root");
    std::cout << "[ok] C: project-local dir anchored at project_root\n";

    // Legacy no-arg call keeps the CWD-relative entry.
    SkillManager legacy;
    legacy.init_after_settings();
    bool found_rel = false;
    for (const auto& d : legacy.skill_directories()) {
        if (d == fs::path(".agents/skills/")) found_rel = true;
    }
    assert(found_rel && "empty project_root keeps legacy CWD-relative dir");
    std::cout << "[ok] C: empty project_root keeps legacy behaviour\n";

    // --- A: refresh_cache picks up mid-session skills ------------------
    make_skill(project_root / ".agents" / "skills" / "late-skill",
               "late-skill");
    assert(sm.get_all_cached_skills().count("late-skill") == 0);
    sm.refresh_cache();
    assert(sm.get_all_cached_skills().count("late-skill") == 1);
    std::cout << "[ok] A: refresh_cache picks up mid-session skill\n";

    // --- discover_skills tool (no directory arg) also sees it ----------
    SkillManager::set_current(&sm);
    DiscoverSkillsTool tool;
    FileIO fio(project_root);
    auto out = tool.execute(nlohmann::json::object(), fio);
    assert(out["status"] == "success");
    bool saw_late = false;
    for (const auto& s : out["skills"]) {
        if (s["name"] == "late-skill") saw_late = true;
    }
    assert(saw_late && "discover_skills must rescan, not serve stale cache");
    std::cout << "[ok] A: discover_skills tool rescans every call\n";

    // A brand-new skill created NOW must appear on the very next call.
    make_skill(project_root / ".agents" / "skills" / "newer-skill",
               "newer-skill");
    out = tool.execute(nlohmann::json::object(), fio);
    bool saw_newer = false;
    for (const auto& s : out["skills"]) {
        if (s["name"] == "newer-skill") saw_newer = true;
    }
    assert(saw_newer && "create-then-rediscover workflow must work");
    std::cout << "[ok] A: create-then-rediscover workflow works\n";

    // --- B: directory argument honoured --------------------------------
    const fs::path other = root / "other-skills";
    make_skill(other / "other-one", "other-one");
    out = tool.execute(nlohmann::json{{"directory", other.string()}}, fio);
    assert(out["status"] == "success");
    assert(out["count"] == 1);
    assert(out["skills"][0]["name"] == "other-one");
    assert(out["skills"][0]["path"].get<std::string>().find("other-one")
           != std::string::npos);
    std::cout << "[ok] B: directory argument scans that dir directly\n";

    SkillManager::set_current(nullptr);
    fs::remove_all(root);
    std::cout << "all skill-discovery fix proofs passed" << std::endl;
    return 0;
}
