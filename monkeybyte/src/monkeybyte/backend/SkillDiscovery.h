#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace monkeybyte {

// One skill discovered on disk (design §4.5 Agent tab). Each immediate
// subdirectory of the skills root that contains a SKILL.md is one skill
// (same convention as daedalus::SkillManager).
struct SkillEntry {
    std::string name;        // front-matter `name:` (fallback: directory name)
    std::string description; // front-matter `description:` (may be empty)
    std::filesystem::path path; // the SKILL.md file
};

// Scans `skills_root` for skills. The default root is daedalus's global
// skills dir (daedalus::Paths::global().global_skills_dir()); tests pass a
// temp dir. GTK-free so it stays unit-testable.
std::vector<SkillEntry> discover_skills(const std::filesystem::path& skills_root);

// discover_skills() against daedalus's global skills dir.
std::vector<SkillEntry> discover_global_skills();

} // namespace monkeybyte
