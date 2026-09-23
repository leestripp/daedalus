#pragma once

#include <optional>
#include <string>
#include <vector>

namespace monkeybyte {

// A project type (Plan.md): a named bundle of a focused system prompt,
// starter-file templates and detection rules. Types are plain files on
// disk, editable in MonkeyByte itself and shareable:
//
//   <types_dir>/<id>.md            front matter + system prompt body
//   <types_dir>/<id>.templates/    starter files for "Add File from Template…"
//
// <id>.md format (tiny front matter — no YAML lib):
//   ---
//   id: film
//   name: Filmmaking
//   description: Screenwriting and film production documents
//   detect: *.fountain, shotlist.md
//   ---
//   (body = the type's system prompt fragment)
struct ProjectType {
    std::string id;                     // "film" (filename stem / front matter)
    std::string name;                   // display name
    std::string description;
    std::vector<std::string> detect_patterns; // "CMakeLists.txt" or "*.fountain"
    std::string system_prompt;          // front-matter body
    std::string source_file;            // the <id>.md this was parsed from
    std::string templates_dir;          // <id>.templates (may not exist)
};

// Scans a types dir and answers "what type is this workspace?".
//
// Workspaces record their type in AGENTS.md as a first-line HTML comment:
//   <!-- monkeybyte:type: film -->
// Detection order: AGENTS.md stamp -> detect-pattern heuristics -> "generic".
class ProjectTypeLibrary {
public:
    // Default: user types dir ($XDG_DATA_HOME/monkeybyte/project-types),
    // seeding missing built-ins from the installed data dir first.
    ProjectTypeLibrary();
    // Explicit dir (tests, Settings rescan of a custom dir). No seeding.
    explicit ProjectTypeLibrary(std::string types_dir);

    static std::string user_types_dir();
    static std::string builtin_types_dir(); // <app data>/project-types

    // Copies built-in types that are MISSING from the user dir (plus their
    // .templates dirs). Never overwrites user files. Returns files copied.
    static int seed_builtins();

    void reload();

    const std::vector<ProjectType>& list() const { return types_; }
    const ProjectType* find(const std::string& id) const;

    // Workspace type per the detection order above. nullptr when the stamp
    // names an unknown id AND no heuristic matches AND there is no generic.
    const ProjectType* detect(const std::string& workspace_dir) const;

    // Reads the stamp from <workspace_dir>/AGENTS.md, if present/valid.
    static std::optional<std::string> read_stamp(const std::string& workspace_dir);
    // Writes or replaces the stamp; creates a minimal AGENTS.md when the
    // workspace has none. False + *error on I/O failure.
    static bool write_stamp(const std::string& workspace_dir,
                            const std::string& type_id,
                            std::string* error = nullptr);

    // "Create Workspace" scaffolding (Plan.md P7): creates `dir`, writes a
    // stamped AGENTS.md, and copies the type's <id>.templates/ starter
    // files (never overwriting existing files). type may be nullptr (no
    // stamp/templates — a plain folder). False + *error on failure.
    static bool scaffold_workspace(const std::string& dir,
                                   const ProjectType* type,
                                   std::string* error = nullptr);

    // Parses one <id>.md file (exposed for tests).
    static ProjectType parse_file(const std::string& path);

    // Rewrites the prompt BODY of a type file, preserving its front matter
    // verbatim (Settings → Workspace "Edit Prompt…", Plan.md P6). False +
    // *error on I/O failure.
    static bool update_prompt(const ProjectType& type,
                              const std::string& new_prompt,
                              std::string* error = nullptr);

    // "CMakeLists.txt" exact, "*.fountain" suffix. Exposed for tests.
    static bool pattern_matches(const std::string& pattern,
                                const std::string& filename);

private:
    std::string dir_;
    std::vector<ProjectType> types_;
};

} // namespace monkeybyte
