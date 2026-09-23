#include "monkeybyte/backend/ProjectTypes.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace monkeybyte {

namespace {

namespace fs = std::filesystem;

constexpr const char* kStampPrefix = "<!-- monkeybyte:type:";
constexpr const char* kStampSuffix = "-->";

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string stamp_line(const std::string& type_id) {
    return std::string(kStampPrefix) + " " + type_id + " " + kStampSuffix;
}

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

} // namespace

std::string ProjectTypeLibrary::user_types_dir() {
    const std::string data_home =
        env_or("XDG_DATA_HOME", env_or("HOME", ".") + "/.local/share");
    return (fs::path(data_home) / "monkeybyte" / "project-types").string();
}

std::string ProjectTypeLibrary::builtin_types_dir() {
    // Resolve relative to the executable (mirrors ThemeManager::data_dir):
    // build tree has data/ next to the binary; prefix installs have
    // <prefix>/share/monkeybyte/data; last resort is the source tree.
    fs::path exe_dir;
    std::error_code ec;
    const auto exe = fs::read_symlink("/proc/self/exe", ec);
    exe_dir = ec ? fs::current_path(ec) : exe.parent_path();

    if (path_exists(exe_dir / "data/project-types"))
        return (exe_dir / "data/project-types").string();
    if (path_exists(exe_dir / "../share/monkeybyte/data/project-types"))
        return (exe_dir / "../share/monkeybyte/data/project-types").string();
    if (path_exists(exe_dir / "../data/project-types"))
        return (exe_dir / "../data/project-types").string();
    return (exe_dir / "data/project-types").string(); // caller checks exists
}

ProjectTypeLibrary::ProjectTypeLibrary() : dir_(user_types_dir()) {
    seed_builtins();
    reload();
}

ProjectTypeLibrary::ProjectTypeLibrary(std::string types_dir)
    : dir_(std::move(types_dir)) {
    reload();
}

int ProjectTypeLibrary::seed_builtins() {
    const fs::path from = builtin_types_dir();
    const fs::path to = user_types_dir();
    std::error_code ec;
    if (!path_exists(from))
        return 0;
    fs::create_directories(to, ec);
    if (ec)
        return 0;

    int copied = 0;
    for (const auto& entry : fs::directory_iterator(from, ec)) {
        const bool is_type = entry.path().extension() == ".md";
        const bool is_templates = entry.path().extension() == ".templates";
        if (!is_type && !is_templates)
            continue;
        const fs::path dest = to / entry.path().filename();
        if (path_exists(dest))
            continue; // never overwrite user files
        fs::copy(entry.path(), dest,
                 fs::copy_options::recursive, ec);
        if (!ec)
            ++copied;
    }
    return copied;
}

void ProjectTypeLibrary::reload() {
    types_.clear();
    std::error_code ec;
    if (!path_exists(dir_))
        return;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (entry.path().extension() != ".md")
            continue;
        types_.push_back(parse_file(entry.path().string()));
    }
    std::sort(types_.begin(), types_.end(),
              [](const ProjectType& a, const ProjectType& b) { return a.id < b.id; });
}

const ProjectType* ProjectTypeLibrary::find(const std::string& id) const {
    for (const auto& t : types_)
        if (t.id == id)
            return &t;
    return nullptr;
}

const ProjectType* ProjectTypeLibrary::detect(const std::string& workspace_dir) const {
    // 1) explicit stamp in AGENTS.md
    if (const auto stamp = read_stamp(workspace_dir))
        if (const ProjectType* t = find(*stamp))
            return t;

    // 2) heuristics: any top-level entry matching a type's detect patterns.
    //    First matching type in sorted-id order wins (deterministic).
    std::error_code ec;
    if (path_exists(workspace_dir)) {
        for (const auto& t : types_) {
            if (t.detect_patterns.empty())
                continue;
            for (const auto& entry : fs::directory_iterator(workspace_dir, ec)) {
                const std::string name = entry.path().filename().string();
                for (const auto& pat : t.detect_patterns)
                    if (pattern_matches(pat, name))
                        return &t;
            }
        }
    }

    // 3) fallback
    return find("generic");
}

bool ProjectTypeLibrary::pattern_matches(const std::string& pattern,
                                         const std::string& filename) {
    if (pattern.size() > 1 && pattern[0] == '*')
        // suffix match: "*.fountain"
        return filename.size() >= pattern.size() - 1 &&
               filename.compare(filename.size() - (pattern.size() - 1),
                                std::string::npos, pattern, 1) == 0;
    return filename == pattern;
}

ProjectType ProjectTypeLibrary::parse_file(const std::string& path) {
    ProjectType t;
    t.source_file = path;
    const fs::path p = path;
    t.id = p.stem().string();
    t.templates_dir = (p.parent_path() / (t.id + ".templates")).string();

    std::ifstream in(path);
    if (!in)
        return t;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();

    // Front matter: file must START with a --- line; key: value lines until
    // the closing ---; everything after is the prompt body.
    std::string body = text;
    if (text.rfind("---\n", 0) == 0 || text.rfind("---\r\n", 0) == 0) {
        std::istringstream lines(text);
        std::string line;
        std::getline(lines, line); // opening ---
        bool closed = false;
        std::size_t body_offset = line.size() + 1;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line == "---") {
                closed = true;
                body_offset += line.size() + 1;
                break;
            }
            body_offset += line.size() + 1;
            const auto colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string key = trim(line.substr(0, colon));
            const std::string value = trim(line.substr(colon + 1));
            if (key == "id" && !value.empty())
                t.id = value;
            else if (key == "name")
                t.name = value;
            else if (key == "description")
                t.description = value;
            else if (key == "detect") {
                std::istringstream pats(value);
                std::string pat;
                while (std::getline(pats, pat, ',')) {
                    pat = trim(pat);
                    if (!pat.empty())
                        t.detect_patterns.push_back(pat);
                }
            }
            // unknown keys ignored on purpose
        }
        body = closed ? text.substr(body_offset) : text;
        // front matter id also relocates the templates dir
        t.templates_dir = (p.parent_path() / (t.id + ".templates")).string();
    }

    t.system_prompt = trim(body);
    if (t.name.empty())
        t.name = t.id;
    return t;
}

bool ProjectTypeLibrary::update_prompt(const ProjectType& type,
                                       const std::string& new_prompt,
                                       std::string* error) {
    std::ifstream in(type.source_file);
    if (!in) {
        if (error) *error = "cannot read " + type.source_file;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    // Front matter spans the opening --- line through the closing --- line;
    // everything after is the body we replace. No front matter => prepend
    // nothing, just write the body (parse_file treats the whole file as the
    // prompt in that case).
    std::string head;
    if (text.rfind("---\n", 0) == 0 || text.rfind("---\r\n", 0) == 0) {
        std::istringstream lines(text);
        std::string line;
        std::size_t off = 0;
        std::getline(lines, line);
        off = line.size() + 1;
        while (std::getline(lines, line)) {
            std::string l = line;
            if (!l.empty() && l.back() == '\r') l.pop_back();
            off += line.size() + 1;
            if (l == "---") {
                head = text.substr(0, off);
                break;
            }
        }
    }

    std::ofstream out(type.source_file, std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + type.source_file;
        return false;
    }
    out << head;
    if (!head.empty()) out << "\n";
    out << new_prompt;
    if (new_prompt.empty() || new_prompt.back() != '\n') out << "\n";
    if (!out.good()) {
        if (error) *error = "write failed for " + type.source_file;
        return false;
    }
    return true;
}

std::optional<std::string>
ProjectTypeLibrary::read_stamp(const std::string& workspace_dir) {
    std::ifstream in(fs::path(workspace_dir) / "AGENTS.md");
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    const auto start = text.find(kStampPrefix);
    if (start == std::string::npos)
        return std::nullopt;
    const auto value_start = start + std::string(kStampPrefix).size();
    const auto end = text.find(kStampSuffix, value_start);
    if (end == std::string::npos)
        return std::nullopt;
    const std::string id = trim(text.substr(value_start, end - value_start));
    if (id.empty())
        return std::nullopt;
    return id;
}

bool ProjectTypeLibrary::write_stamp(const std::string& workspace_dir,
                                     const std::string& type_id,
                                     std::string* error) {
    const fs::path path = fs::path(workspace_dir) / "AGENTS.md";
    const std::string stamp = stamp_line(type_id);

    std::string content;
    if (path_exists(path)) {
        std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        content = ss.str();
    }

    std::string out;
    const auto start = content.find(kStampPrefix);
    if (!content.empty() && start != std::string::npos) {
        // replace the whole existing stamp line
        const auto line_start = content.rfind('\n', start);
        const auto end = content.find(kStampSuffix, start);
        const auto line_end = end == std::string::npos
                                  ? content.size()
                                  : content.find('\n', end);
        const std::size_t ls = line_start == std::string::npos ? 0 : line_start + 1;
        const std::size_t le = line_end == std::string::npos ? content.size() : line_end;
        out = content.substr(0, ls) + stamp + content.substr(le);
    } else if (!content.empty()) {
        out = stamp + "\n\n" + content;
    } else {
        out = stamp + "\n\n# Project\n\n"
              "_Describe this project for AI agents here (or run /init in the "
              "chat panel to generate it)._\n";
    }

    std::ofstream ofs(path, std::ios::trunc);
    if (!ofs) {
        if (error)
            *error = "cannot write " + path.string();
        return false;
    }
    ofs << out;
    if (!ofs.good()) {
        if (error)
            *error = "write failed for " + path.string();
        return false;
    }
    return true;
}

bool ProjectTypeLibrary::scaffold_workspace(const std::string& dir,
                                            const ProjectType* type,
                                            std::string* error) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        if (error) *error = "cannot create " + dir + ": " + ec.message();
        return false;
    }
    if (type) {
        if (!write_stamp(dir, type->id, error))
            return false;
        // Starter files from <id>.templates/ — skip existing, never clobber.
        if (path_exists(type->templates_dir)) {
            for (const auto& entry :
                 fs::directory_iterator(type->templates_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const fs::path dest =
                    fs::path(dir) / entry.path().filename();
                if (path_exists(dest)) continue;
                fs::copy_file(entry.path(), dest, ec);
                if (ec) {
                    if (error)
                        *error = "template copy failed: " + ec.message();
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace monkeybyte
