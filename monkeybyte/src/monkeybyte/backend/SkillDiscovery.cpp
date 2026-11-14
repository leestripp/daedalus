#include "monkeybyte/backend/SkillDiscovery.h"

#include <daedalus/paths.h>

#include <fstream>
#include <sstream>

namespace monkeybyte {

namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string unquote(std::string s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\'')))
        s = s.substr(1, s.size() - 2);
    return s;
}

// Extracts the YAML front matter (between the first "---" line and the
// next "---" line) and pulls scalar `name:` / `description:` values out
// of it. Same file convention as daedalus::SkillManager::parse_frontmatter.
SkillEntry parse_skill_md(const std::filesystem::path& file,
                          const std::string& dir_name) {
    SkillEntry entry;
    entry.name = dir_name;
    entry.path = file;

    std::ifstream in(file);
    if (!in) return entry;

    std::string line;
    bool in_front = false;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t == "---") {
            if (!in_front) { in_front = true; continue; }
            break; // closing delimiter
        }
        if (!in_front) break; // no front matter — keep dir-name fallback

        const auto colon = t.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = trim(t.substr(0, colon));
        const std::string value = unquote(trim(t.substr(colon + 1)));
        if (key == "name" && !value.empty()) entry.name = value;
        else if (key == "description") entry.description = value;
    }
    return entry;
}

} // namespace

std::vector<SkillEntry> discover_skills(const std::filesystem::path& skills_root) {
    std::vector<SkillEntry> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(skills_root, ec)) return out;
    for (const auto& dir :
         std::filesystem::directory_iterator(skills_root, ec)) {
        if (!dir.is_directory()) continue;
        const auto skill_md = dir.path() / "SKILL.md";
        if (!std::filesystem::is_regular_file(skill_md, ec)) continue;
        out.push_back(parse_skill_md(skill_md, dir.path().filename().string()));
    }
    return out;
}

std::vector<SkillEntry> discover_global_skills() {
    return discover_skills(daedalus::Paths::global().global_skills_dir());
}

} // namespace monkeybyte
