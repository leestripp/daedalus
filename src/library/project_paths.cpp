/**
 * @file project_paths.cpp
 * @brief Per-project, per-agent filesystem location resolver.
 *
 * Resolves `<project_root>/.daedalus[-<name>]/` for every
 * (project_root, agent_name) pair. The same ProjectPaths logic
 * is shared with the legacy `resolve_agent_dir()` helper in
 * `src/library/default_paths.cpp` so all per-project path
 * resolution lives in one place.
 */
#include "daedalus/project_paths.h"

#include "daedalus/default_paths.h"  // is_valid_agent_name()

#include <system_error>

namespace daedalus {

namespace fs = std::filesystem;

namespace {
constexpr std::string_view kAgentDirNewPrefix = ".daedalus";
constexpr std::string_view kAgentDirNewNamedPrefix = ".daedalus-";

fs::path resolve_agent_dir_impl(
	const fs::path& project_root,
	std::string_view agent_name )
{
	fs::path new_path = agent_name.empty()
	                        ? project_root / std::string( kAgentDirNewPrefix )
	                        : project_root / ( std::string( kAgentDirNewNamedPrefix ) + std::string( agent_name ) );

	// Caller creates the path on first write; we don't probe here.
	return new_path;
}
}  // namespace

ProjectPaths::ProjectPaths( fs::path project_root, std::string agent_name )
	: m_project_root( std::move( project_root ) )
	, m_agent_name( std::move( agent_name ) )
{
	if ( !is_valid_agent_name( m_agent_name ) ) {
		throw std::invalid_argument(
			"ProjectPaths: invalid agent_name (see is_valid_agent_name rules)" );
	}
}

const fs::path& ProjectPaths::project_root() const noexcept
{
	return m_project_root;
}

std::string_view ProjectPaths::agent_name() const noexcept
{
	return m_agent_name;
}

const fs::path& ProjectPaths::agent_dir() const noexcept
{
	if ( !m_agent_dir_resolved ) {
		m_agent_dir_cache = resolve_agent_dir_impl( m_project_root, m_agent_name );
		m_agent_dir_resolved = true;
	}
	return m_agent_dir_cache;
}

// --- Concrete project-local file paths ---

fs::path ProjectPaths::memory_dir() const
{
	return agent_dir() / "memory";
}

fs::path ProjectPaths::memory_file() const
{
	return memory_dir() / "memory.sqlite";
}

fs::path ProjectPaths::memory_file_json() const
{
	return memory_dir() / "memory.json";
}

fs::path ProjectPaths::sessions_dir() const
{
	return agent_dir() / "sessions";
}

fs::path ProjectPaths::session_dir( std::string_view session_id ) const
{
	return sessions_dir() / std::string( session_id );
}

fs::path ProjectPaths::scratch_file( std::string_view session_id ) const
{
	return session_dir( session_id ) / "scratch.json";
}

fs::path ProjectPaths::skills_active_dir() const
{
	return agent_dir() / "skills" / "active";
}

fs::path ProjectPaths::agents_subdir() const
{
	return agent_dir() / "agents";
}

fs::path ProjectPaths::lock_file() const
{
	return agent_dir() / ".lock";
}

std::vector<fs::path> ProjectPaths::skills_search_path( const Paths& paths ) const
{
	// Highest priority first — see project_paths.h docs.
	std::vector<fs::path> out;
	out.push_back( skills_active_dir() );
	out.push_back( paths.global_skills_dir() );
	for ( const auto& d : paths.system_data_dirs() ) {
		out.push_back( d / "skills" / "default" );
	}
	return out;
}

}  // namespace daedalus