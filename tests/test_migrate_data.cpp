// SPDX-License-Identifier: BSL-1.0
// tests/test_migrate_data.cpp
//
// Plan.md Phase 5 acceptance: `daedalus-cli --migrate-data` CLI
// migrates state correctly. We test the underlying MigratePlan logic
// directly (the CLI subcommand is a one-line wrapper around it).
//
// Tests:
//   1. Plan is non-empty when legacy ~/.daedalus/ has files.
//   2. Plan is correctly empty when legacy is absent.
//   3. Apply copies all known file kinds to their XDG locations.
//   4. After apply, the legacy directory is UNTOUCHED.
//   5. Plan respects $XDG_CONFIG_HOME / $XDG_DATA_HOME overrides.
//   6. --from DIR override targets an arbitrary legacy root.
//   7. Dry-run does not create any destination files.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include <daedalus/paths.h>

#include "../demos/daedalus-cli/migrate.h"

namespace fs = std::filesystem;

namespace {

// RAII env setter
class ScopedEnv
{
public:
	ScopedEnv( const char* name, const std::string& value ) : m_name( name )
	{
		const char* prev = std::getenv( name );
		m_had_prev = ( prev != nullptr );
		if ( m_had_prev ) m_prev = prev;
		if ( value.empty() ) {
			::unsetenv( name );
		} else {
			::setenv( name, value.c_str(), 1 );
		}
	}
	~ScopedEnv()
	{
		if ( m_had_prev ) {
			::setenv( m_name, m_prev.c_str(), 1 );
		} else {
			::unsetenv( m_name );
		}
	}
	ScopedEnv( const ScopedEnv& ) = delete;
	ScopedEnv& operator=( const ScopedEnv& ) = delete;

private:
	const char* m_name;
	std::string m_prev;
	bool m_had_prev;
};

fs::path fresh_dir( const char* tag )
{
	auto dir = fs::temp_directory_path() / "daedalus_migrate_test" / tag;
	std::error_code ec;
	fs::remove_all( dir, ec );
	fs::create_directories( dir );
	return dir;
}

void write_text( const fs::path& p, const std::string& contents )
{
	fs::create_directories( p.parent_path() );
	std::ofstream f( p );
	f << contents;
}

// Kept for future use — currently unused.
[[maybe_unused]]
static size_t existing_count( const daedalus_cli::MigratePlan& plan )
{
    size_t n = 0;
    for ( const auto& e : plan.entries ) {
        if ( e.from_exists ) ++n;
    }
    return n;
}

}  // namespace

// =============================================================================
// 1. Plan non-empty when legacy has files
// =============================================================================
static void test_plan_nonempty_when_legacy_present()
{
	std::cout << "[1] Plan non-empty when legacy has files... ";
	auto home = fresh_dir( "p1" );
	auto xdg_c = fresh_dir( "p1_cfg" );
	auto xdg_d = fresh_dir( "p1_data" );

	ScopedEnv e_home( "HOME", home.string() );
	ScopedEnv e_xdg_c( "XDG_CONFIG_HOME", xdg_c.string() );
	ScopedEnv e_xdg_d( "XDG_DATA_HOME", xdg_d.string() );
	ScopedEnv e_dh( "DAEDALUS_HOME", "" );

	write_text( home / ".daedalus" / "settings" / "settings.json",
	            "{\"default_model\":\"x\"}" );
	write_text( home / ".daedalus" / "mcp" / "servers.json", "{}" );
	write_text( home / ".daedalus" / "skills" / "installed" / "readme.md",
	            "hi" );

	daedalus_cli::MigrateOptions opt;
	auto plan = daedalus_cli::compute_migration_plan( opt );

	assert( existing_count( plan ) >= 3 );  // settings, mcp/servers, skills
	std::cout << "ok\n";

	fs::remove_all( home );
	fs::remove_all( xdg_c );
	fs::remove_all( xdg_d );
}

// =============================================================================
// 2. Plan empty when legacy absent
// =============================================================================
static void test_plan_empty_when_legacy_absent()
{
	std::cout << "[2] Plan empty when legacy absent... ";
	auto home = fresh_dir( "p2" );
	ScopedEnv e_home( "HOME", home.string() );
	ScopedEnv e_dh( "DAEDALUS_HOME", "" );

	daedalus_cli::MigrateOptions opt;
	auto plan = daedalus_cli::compute_migration_plan( opt );
	assert( existing_count( plan ) == 0 );
	std::cout << "ok\n";

	fs::remove_all( home );
}

// =============================================================================
// 3. Apply copies all known file kinds
// =============================================================================
static void test_apply_copies_all()
{
	std::cout << "[3] Apply copies all entries... ";
	auto home = fresh_dir( "p3" );
	auto xdg_c = fresh_dir( "p3_cfg" );
	auto xdg_d = fresh_dir( "p3_data" );

	ScopedEnv e_home( "HOME", home.string() );
	ScopedEnv e_xdg_c( "XDG_CONFIG_HOME", xdg_c.string() );
	ScopedEnv e_xdg_d( "XDG_DATA_HOME", xdg_d.string() );
	ScopedEnv e_dh( "DAEDALUS_HOME", "" );

	const std::string settings_src = R"({"default_model":"minimax-m2.7"})";
	const std::string mcp_src       = R"({"mcpServers":{}})";
	const std::string env_src       = "API_KEY=secret\n";
	write_text( home / ".daedalus" / "settings" / "settings.json", settings_src );
	write_text( home / ".daedalus" / "mcp" / "servers.json", mcp_src );
	write_text( home / ".daedalus" / "mcp" / ".env", env_src );
	write_text( home / ".daedalus" / "skills" / "pack" / "SKILL.md", "# skill\n" );

	daedalus_cli::MigrateOptions opt;
	opt.dry_run = false;
	auto plan = daedalus_cli::compute_migration_plan( opt );
	int rc = daedalus_cli::apply_migration_plan( plan, false );
	assert( rc >= 3 );
	(void)rc;

	// Verify each destination contains the expected bytes.
	{
		std::ifstream f( xdg_c / "daedalus" / "config.json" );
		std::stringstream ss;
		ss << f.rdbuf();
		assert( ss.str() == settings_src );
	}
	{
		std::ifstream f( xdg_c / "daedalus" / "mcp" / "servers.json" );
		std::stringstream ss;
		ss << f.rdbuf();
		assert( ss.str() == mcp_src );
	}
	{
		std::ifstream f( xdg_c / "daedalus" / "mcp" / ".env" );
		std::stringstream ss;
		ss << f.rdbuf();
		assert( ss.str() == env_src );
	}
	assert( fs::exists( xdg_d / "daedalus" / "skills" / "installed" / "pack" / "SKILL.md" ) );

	std::cout << "ok\n";

	fs::remove_all( home );
	fs::remove_all( xdg_c );
	fs::remove_all( xdg_d );
}

// =============================================================================
// 4. Apply leaves legacy untouched
// =============================================================================
static void test_apply_preserves_legacy()
{
	std::cout << "[4] Apply leaves legacy intact... ";
	auto home = fresh_dir( "p4" );
	auto xdg_c = fresh_dir( "p4_cfg" );
	auto xdg_d = fresh_dir( "p4_data" );

	ScopedEnv e_home( "HOME", home.string() );
	ScopedEnv e_xdg_c( "XDG_CONFIG_HOME", xdg_c.string() );
	ScopedEnv e_xdg_d( "XDG_DATA_HOME", xdg_d.string() );
	ScopedEnv e_dh( "DAEDALUS_HOME", "" );

	write_text( home / ".daedalus" / "settings" / "settings.json",
	            "{\"k\":\"v\"}" );

	daedalus_cli::MigrateOptions opt;
	opt.dry_run = false;
	auto plan = daedalus_cli::compute_migration_plan( opt );
	daedalus_cli::apply_migration_plan( plan, false );

	// Legacy must STILL exist after apply.
	assert( fs::exists( home / ".daedalus" / "settings" / "settings.json" ) );
	std::cout << "ok\n";

	fs::remove_all( home );
	fs::remove_all( xdg_c );
	fs::remove_all( xdg_d );
}

// =============================================================================
// 5. Plan respects XDG env-var overrides
// =============================================================================
static void test_plan_respects_xdg_overrides()
{
	std::cout << "[5] Plan reflects XDG env-var overrides... ";
	auto home = fresh_dir( "p5" );
	ScopedEnv e_home( "HOME", home.string() );
	ScopedEnv e_dh( "DAEDALUS_HOME", "" );

	daedalus_cli::MigrateOptions opt;
	ScopedEnv e_xdg_c( "XDG_CONFIG_HOME", "/tmp/override_cfg" );
	ScopedEnv e_xdg_d( "XDG_DATA_HOME", "/tmp/override_data" );

	auto plan = daedalus_cli::compute_migration_plan( opt );
	assert( plan.xdg_config_home == "/tmp/override_cfg" );
	assert( plan.xdg_data_home == "/tmp/override_data" );
	std::cout << "ok\n";

	fs::remove_all( home );
}

// =============================================================================
// 6. --from DIR override targets arbitrary legacy root
// =============================================================================
static void test_from_override_targets_arbitrary_root()
{
	std::cout << "[6] --from override... ";
	auto arbitrary = fresh_dir( "p6_arbitrary" );

	write_text( arbitrary / "settings" / "settings.json", "{}" );

	daedalus_cli::MigrateOptions opt;
	opt.from = arbitrary;
	auto plan = daedalus_cli::compute_migration_plan( opt );

	assert( plan.from_root == arbitrary );
	size_t n = 0;
	for ( const auto& e : plan.entries ) {
		if ( e.from_exists
		     && e.from.string().find( arbitrary.string() ) != std::string::npos ) {
			++n;
		}
	}
	assert( n >= 1 );
	(void)n;
	std::cout << "ok\n";

	fs::remove_all( arbitrary );
}

// =============================================================================
// 7. Dry-run does not write destinations
// =============================================================================
static void test_dry_run_preserves_destinations()
{
	std::cout << "[7] Dry-run does not write destinations... ";
	auto home = fresh_dir( "p7" );
	auto xdg_c = fresh_dir( "p7_cfg" );
	auto xdg_d = fresh_dir( "p7_data" );

	ScopedEnv e_home( "HOME", home.string() );
	ScopedEnv e_xdg_c( "XDG_CONFIG_HOME", xdg_c.string() );
	ScopedEnv e_xdg_d( "XDG_DATA_HOME", xdg_d.string() );
	ScopedEnv e_dh( "DAEDALUS_HOME", "" );

	write_text( home / ".daedalus" / "settings" / "settings.json", "{}" );

	daedalus_cli::MigrateOptions opt;
	opt.dry_run = true;  // ← default; explicit here for clarity
	auto plan = daedalus_cli::compute_migration_plan( opt );
	int rc = daedalus_cli::apply_migration_plan( plan, false );
	assert( rc >= 1 );  // counts but does NOT copy
	(void)rc;

	// Critical assertion: destination must NOT exist.
	assert( !fs::exists( xdg_c / "daedalus" / "config.json" ) );
	std::cout << "ok\n";

	fs::remove_all( home );
	fs::remove_all( xdg_c );
	fs::remove_all( xdg_d );
}

int main()
{
	test_plan_nonempty_when_legacy_present();
	test_plan_empty_when_legacy_absent();
	test_apply_copies_all();
	test_apply_preserves_legacy();
	test_plan_respects_xdg_overrides();
	test_from_override_targets_arbitrary_root();
	test_dry_run_preserves_destinations();
	std::cout << "All test_migrate_data tests passed.\n";
	return 0;
}