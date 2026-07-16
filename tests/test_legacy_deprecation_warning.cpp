// SPDX-License-Identifier: BSL-1.0
// tests/test_legacy_deprecation_warning.cpp
//
// Plan.md Phase 4 acceptance: on startup, if ~/.daedalus/ exists
// AND $DAEDALUS_HOME / $XDG_DATA_HOME are unset, emit a one-time
// deprecation warning via the sink (NOT to stderr — no-stdio rule).
//
// Tests:
//   1. With legacy ~/.daedalus/ present and no override env vars,
//      Agent::initialize() emits exactly one [DEPRECATION] warning
//      via sink_->on_log(LogLevel::Warn, ...).
//   2. With $DAEDALUS_HOME set, no warning (override wins).
//   3. With $XDG_DATA_HOME set, no warning (XDG wins).
//   4. With legacy ~/.daedalus/ ABSENT, no warning (clean install).
//   5. The warning text mentions Plan.md §3.4 so users can grep docs.

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <daedalus.h>

namespace fs = std::filesystem;

namespace {

// Captures every on_log call so we can grep them.
struct CapturingSink : public daedalus::AgentSink
{
	std::vector<std::pair<daedalus::LogLevel, std::string>> events;

	void on_log( daedalus::LogLevel lvl, std::string_view msg ) noexcept override
	{
		events.emplace_back( lvl, std::string( msg ) );
	}
};

size_t count_deprecation_warnings( const CapturingSink& sink )
{
	size_t n = 0;
	for ( const auto& [lvl, msg] : sink.events ) {
		(void)lvl;
		if ( msg.find( "[DEPRECATION]" ) != std::string::npos ) ++n;
	}
	return n;
}

fs::path fresh_dir( const char* tag )
{
	auto dir = fs::temp_directory_path() / "daedalus_legacy_test" / tag;
	std::error_code ec;
	fs::remove_all( dir, ec );
	fs::create_directories( dir );
	return dir;
}

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

// Make a minimal AgentConfig that won't try to talk to an LLM.
// We use the absolute minimum so AgentImpl::initialize() can complete
// without needing API keys. The deprecation check happens BEFORE the
// LLM spin-up, so we don't even need cfg_.model_name / cfg_.api_key.
daedalus::AgentConfig make_minimal_cfg( const fs::path& project_root,
                                        const fs::path& daedalus_home )
{
	daedalus::AgentConfig cfg;
	cfg.project_root = project_root;
	cfg.daedalus_home = daedalus_home;
	cfg.session_id = "legacy-deprecation-test";
	cfg.memory_enabled = false;  // don't touch <project>/.daedalus/memory/
	return cfg;
}

}  // namespace

// =============================================================================
// 1. Legacy ~/.daedalus/ present, no override → warning fires
// =============================================================================
static void test_legacy_home_emits_warning()
{
	std::cout << "[1] Legacy ~/.daedalus/ present → warning fires... ";

	auto project_root = fresh_dir( "p1" );
	auto fake_home = fresh_dir( "h1" );
	fs::create_directories( fake_home / ".daedalus" );

	ScopedEnv e_daedalus_home( "DAEDALUS_HOME", "" );
	ScopedEnv e_xdg_data( "XDG_DATA_HOME", "" );
	ScopedEnv e_home( "HOME", fake_home.string() );

	auto sink = std::make_shared<CapturingSink>();
	daedalus::AgentConfig cfg = make_minimal_cfg(
		project_root, fake_home / ".daedalus" );

	daedalus::Agent agent( std::move( cfg ), sink );
	agent.initialize();

	assert( count_deprecation_warnings( *sink ) == 1 );
	std::cout << "ok\n";

	fs::remove_all( project_root );
	fs::remove_all( fake_home );
}

// =============================================================================
// 2. $DAEDALUS_HOME set → no warning (override wins)
// =============================================================================
static void test_daedalus_home_override_silences()
{
	std::cout << "[2] $DAEDALUS_HOME set → no warning... ";

	auto project_root = fresh_dir( "p2" );
	auto fake_home = fresh_dir( "h2" );
	auto daedalus_home = fresh_dir( "daedalus_home_2" );
	fs::create_directories( fake_home / ".daedalus" );  // legacy exists

	ScopedEnv e_daedalus_home( "DAEDALUS_HOME", daedalus_home.string() );
	ScopedEnv e_xdg_data( "XDG_DATA_HOME", "" );
	ScopedEnv e_home( "HOME", fake_home.string() );

	auto sink = std::make_shared<CapturingSink>();
	daedalus::AgentConfig cfg = make_minimal_cfg(
		project_root, daedalus_home );

	daedalus::Agent agent( std::move( cfg ), sink );
	agent.initialize();

	assert( count_deprecation_warnings( *sink ) == 0 );
	std::cout << "ok\n";

	fs::remove_all( project_root );
	fs::remove_all( fake_home );
	fs::remove_all( daedalus_home );
}

// =============================================================================
// 3. $XDG_DATA_HOME set → no warning (XDG wins)
// =============================================================================
static void test_xdg_data_home_silences()
{
	std::cout << "[3] $XDG_DATA_HOME set → no warning... ";

	auto project_root = fresh_dir( "p3" );
	auto fake_home = fresh_dir( "h3" );
	fs::create_directories( fake_home / ".daedalus" );  // legacy exists

	ScopedEnv e_daedalus_home( "DAEDALUS_HOME", "" );
	ScopedEnv e_xdg_data( "XDG_DATA_HOME", "/tmp/xdg-data-override" );
	ScopedEnv e_home( "HOME", fake_home.string() );

	auto sink = std::make_shared<CapturingSink>();
	daedalus::AgentConfig cfg = make_minimal_cfg(
		project_root, "/tmp/xdg-data-override/daedalus" );

	daedalus::Agent agent( std::move( cfg ), sink );
	agent.initialize();

	assert( count_deprecation_warnings( *sink ) == 0 );
	std::cout << "ok\n";

	fs::remove_all( project_root );
	fs::remove_all( fake_home );
}

// =============================================================================
// 4. Legacy ~/.daedalus/ absent → no warning (clean install)
// =============================================================================
static void test_no_legacy_no_warning()
{
	std::cout << "[4] No legacy ~/.daedalus/ → no warning... ";

	auto project_root = fresh_dir( "p4" );
	auto fake_home = fresh_dir( "h4" );
	// NO .daedalus under fake_home

	ScopedEnv e_daedalus_home( "DAEDALUS_HOME", "" );
	ScopedEnv e_xdg_data( "XDG_DATA_HOME", "" );
	ScopedEnv e_home( "HOME", fake_home.string() );

	auto sink = std::make_shared<CapturingSink>();
	daedalus::AgentConfig cfg = make_minimal_cfg(
		project_root, fake_home / ".local" / "share" / "daedalus" );

	daedalus::Agent agent( std::move( cfg ), sink );
	agent.initialize();

	assert( count_deprecation_warnings( *sink ) == 0 );
	std::cout << "ok\n";

	fs::remove_all( project_root );
	fs::remove_all( fake_home );
}

// =============================================================================
// 5. Warning text mentions Plan.md so users can grep docs
// =============================================================================
static void test_warning_mentions_plan()
{
	std::cout << "[5] Warning text references Plan.md... ";

	auto project_root = fresh_dir( "p5" );
	auto fake_home = fresh_dir( "h5" );
	fs::create_directories( fake_home / ".daedalus" );

	ScopedEnv e_daedalus_home( "DAEDALUS_HOME", "" );
	ScopedEnv e_xdg_data( "XDG_DATA_HOME", "" );
	ScopedEnv e_home( "HOME", fake_home.string() );

	auto sink = std::make_shared<CapturingSink>();
	daedalus::AgentConfig cfg = make_minimal_cfg(
		project_root, fake_home / ".daedalus" );

	daedalus::Agent agent( std::move( cfg ), sink );
	agent.initialize();

	bool found = false;
	for ( const auto& [lvl, msg] : sink->events ) {
		(void)lvl;
		if ( msg.find( "[DEPRECATION]" ) != std::string::npos
		     && msg.find( "Plan.md" ) != std::string::npos ) {
			found = true;
			break;
		}
	}
	assert( found );
	(void)found;
	std::cout << "ok\n";

	fs::remove_all( project_root );
	fs::remove_all( fake_home );
}

int main()
{
	test_legacy_home_emits_warning();
	test_daedalus_home_override_silences();
	test_xdg_data_home_silences();
	test_no_legacy_no_warning();
	test_warning_mentions_plan();
	std::cout << "All test_legacy_deprecation_warning tests passed.\n";
	return 0;
}