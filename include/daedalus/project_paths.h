/**
 * @file daedalus/project_paths.h
 * @brief Per-project filesystem location resolution (Plan.md §3.3.2).
 *
 * Every concrete file path in this class derives from `agent_dir()`,
 * which is namespaced by `agent_name`. Two `Agent` instances in the
 * same project with different `agent_name` values resolve to
 * different `agent_dir()` paths — guaranteed by filesystem layout.
 *
 * Skills search priority (highest first):
 *   1. `<project_root>/.daedalus[-<name>]/skills/active/` — project-local
 *   2. `<user_data_root>/skills/installed/`              — user-installed
 *   3. `<system_data_dirs[0]>/skills/default/`           — system-shipped
 *
 * Memory, sessions, scratch, project config, and the lock file ALL
 * live under `agent_dir()`. Nothing here ever bleeds into another
 * project (different `project_root`) or another agent in the same
 * project (different `agent_name`).
 */
#pragma once

#include <daedalus/paths.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace daedalus {

/**
 * @brief Per-project, per-agent filesystem locations.
 *
 * Constructed cheaply; all path resolution is lazy and cached
 * internally. Thread-affinity: instances are intended to be owned
 * by one `Agent`; sharing across threads requires external
 * synchronisation.
 */
class ProjectPaths
{
public:
	/// Construct with the host's project root and (optional) agent
	/// name. `agent_name == ""` means "default namespace"
	/// (`<project_root>/.daedalus/`).
	ProjectPaths( std::filesystem::path project_root,
	              std::string agent_name = {} );

	/// Copyable / movable. Cached paths stay valid.
	ProjectPaths( const ProjectPaths& ) = default;
	ProjectPaths( ProjectPaths&& ) noexcept = default;
	ProjectPaths& operator=( const ProjectPaths& ) = default;
	ProjectPaths& operator=( ProjectPaths&& ) noexcept = default;

	// --- Inputs (cheap accessors) ---

	const std::filesystem::path& project_root() const noexcept;
	std::string_view agent_name() const noexcept;

	// --- Agent namespace dir (the boundary) ---

	/// `<project_root>/.daedalus/` (empty agent_name) or
	/// `<project_root>/.daedalus-<name>/`.
	/// Falls back to the legacy `.codemax[-<name>]/` path for READS
	/// only when the new path doesn't exist (Phase 12 backward-compat).
	const std::filesystem::path& agent_dir() const noexcept;

	// --- Concrete project-local file paths ---

	std::filesystem::path memory_dir() const;        ///< `agent_dir() / "memory"`
	std::filesystem::path memory_file() const;       ///< `memory_dir()  / "memory.sqlite"`
	std::filesystem::path memory_file_json() const;  ///< legacy `memory_dir() / "memory.json"`
	std::filesystem::path sessions_dir() const;      ///< `agent_dir() / "sessions"`
	std::filesystem::path session_dir( std::string_view session_id ) const;
	std::filesystem::path scratch_file( std::string_view session_id ) const;
	std::filesystem::path project_config_file() const;
	std::filesystem::path skills_active_dir() const;
	std::filesystem::path agents_subdir() const;
	std::filesystem::path lock_file() const;

	// --- Skills search path (cross-tier) ---

	/// Returns directories in priority order, HIGHEST first.
	/// Includes project-local, user-installed, and system-shipped
	/// skill dirs. The library probes them in this order at skill
	/// discovery time.
	std::vector<std::filesystem::path> skills_search_path(
		const Paths& paths ) const;

private:
	std::filesystem::path m_project_root;
	std::string m_agent_name;
	mutable std::filesystem::path m_agent_dir_cache;
	mutable bool m_agent_dir_resolved = false;
};

}  // namespace daedalus
