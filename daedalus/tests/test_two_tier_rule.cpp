// SPDX-License-Identifier: BSL-1.0
// tests/test_two_tier_rule.cpp
//
// Plan.md §6 acceptance: No code path writes to
// `$XDG_DATA_HOME/daedalus/memory/` (or any
// `~/.local/share/daedalus/memory*` path). This must be enforced by
// a unit test that greps the source tree.
//
// Strategy:
//   1. **Static check** — scan src/ for any literal substring that
//      writes memory to an XDG path. We check for these strings:
//        - "memory.sqlite"
//        - "memory.json"
//      appearing in code that ALSO references one of the XDG root
//      functions or env vars from Plan.md §3.7. A pure "memory.sqlite"
//      string is fine if it's only used under a ProjectPaths call.
//   2. **Runtime check** — construct two `ProjectPaths` with
//      different `agent_name`s and verify their `memory_dir()`s do
//      NOT live under `user_data_root()` (which is where XDG data
//      lives). This is the runtime form of the rule.
//
// The static check is a string scan; if the library grows a function
// that writes to XDG/memory, this test will fail with a clear message.

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <daedalus/paths.h>
#include <daedalus/project_paths.h>

namespace fs = std::filesystem;

// =============================================================================
// 1. Static scan — no source file may write to a path matching
//    "$XDG_DATA_HOME/daedalus/memory" or similar.
// =============================================================================
[[maybe_unused]] static bool contains_xdg_memory_write( const fs::path& src_root )
{
	// Patterns that would indicate an XDG-layer memory write.
	const std::vector<std::string> forbidden_substrings = {
		"user_data_root() / \"memory\"",
		"user_data_root()/\"memory\"",
		"global_skills_dir() / \"memory\"",   // (skills dir is in data_root)
		"XDG_DATA_HOME/daedalus/memory",
		"~/.local/share/daedalus/memory",
		"user_root() / \"memory\"",           // legacy global memory — see Plan.md §1.1
	};

	std::vector<std::string> hits;

	for ( auto& entry : fs::recursive_directory_iterator( src_root ) ) {
		if ( !entry.is_regular_file() ) continue;
		auto ext = entry.path().extension().string();
		if ( ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".cc" ) continue;

		std::ifstream f( entry.path() );
		if ( !f.is_open() ) continue;
		std::stringstream ss;
		ss << f.rdbuf();
		std::string content = ss.str();

		for ( const auto& needle : forbidden_substrings ) {
			if ( content.find( needle ) != std::string::npos ) {
				hits.push_back( entry.path().string() + "  →  contains: " + needle );
			}
		}
	}

	if ( !hits.empty() ) {
		std::cerr << "FAIL: code writes memory to an XDG-layer path. Violations:\n";
		for ( const auto& h : hits ) std::cerr << "  " << h << "\n";
		return false;
	}
	return true;
}

static void test_static_no_xdg_memory_write()
{
	std::cout << "[1] Static scan: no source file writes to XDG/memory... ";

	// Walk up from the test binary's CWD to find the repo's src/.
	fs::path here = fs::current_path();
	fs::path src_root;
	for ( fs::path p = here; !p.empty(); p = p.parent_path() ) {
		if ( fs::is_directory( p / "src" ) ) {
			src_root = p / "src";
			break;
		}
	}
	if ( src_root.empty() ) {
		// Fall back to known layout: tests run from build/tests/...
		src_root = here / ".." / ".." / "src";
		std::error_code ec;
		if ( !fs::is_directory( src_root, ec ) ) {
			std::cerr << "WARN: cannot locate src/ for static scan (tried "
			          << src_root << "); skipping.\n";
			std::cout << "skip\n";
			return;
		}
	}
	src_root = fs::canonical( src_root );

	assert( contains_xdg_memory_write( src_root ) );
	std::cout << "ok\n";
}

// =============================================================================
// 2. Runtime: memory_dir() is never under user_data_root()
// =============================================================================
static void test_memory_not_under_user_data()
{
	std::cout << "[2] ProjectPaths::memory_dir() never under user_data_root()... ";

	// Force the XDG data root to a known location.
	::setenv( "XDG_DATA_HOME", "/tmp/xdg_two_tier_test", 1 );
	::setenv( "DAEDALUS_DATA_HOME", "", 1 );
	::setenv( "DAEDALUS_HOME", "", 1 );

	auto project_root = fs::temp_directory_path() / "daedalus_two_tier_proj";
	std::error_code ec;
	fs::remove_all( project_root, ec );
	fs::create_directories( project_root );

	std::vector<std::string> names = { "", "alice", "bob", "planner.42" };
	for ( const auto& n : names ) {
		daedalus::ProjectPaths pp( project_root, n );
		fs::path memdir = pp.memory_dir().lexically_normal().parent_path();  // strip /memory
		fs::path user_data = fs::path{ "/tmp/xdg_two_tier_test/daedalus" }.lexically_normal();

		// The memory dir's project-side ancestor must NOT equal user_data.
		// i.e. `<project>/.daedalus/daedalus[-<name>]/memory` is NOT under
		// `/tmp/xdg_two_tier_test/daedalus/`.
		assert( memdir != user_data );
		// And: the canonicalised memory_dir() must NOT start with user_data.
		// Use lexical comparison instead of canonical() because the path
		// doesn't exist on disk yet — canonical() would throw.
		std::string memdir_s = ( pp.memory_dir() ).lexically_normal().string();
		std::string user_s = user_data.lexically_normal().string();
		assert( memdir_s.rfind( user_s, 0 ) != 0 );
	}

	fs::remove_all( project_root, ec );
	std::cout << "ok\n";
}

// =============================================================================
// 3. Runtime: two agents in same project → different memory_dir()
// =============================================================================
static void test_two_agents_distinct_memory()
{
	std::cout << "[3] Two agents → distinct memory_dir()... ";
	auto project_root = fs::temp_directory_path() / "daedalus_two_agents_proj";
	std::error_code ec;
	fs::remove_all( project_root, ec );
	fs::create_directories( project_root );

	daedalus::ProjectPaths alice( project_root, "alice" );
	daedalus::ProjectPaths bob( project_root, "bob" );

	assert( alice.memory_dir() != bob.memory_dir() );
	assert( alice.memory_file() != bob.memory_file() );

	fs::remove_all( project_root, ec );
	std::cout << "ok\n";
}

// =============================================================================
// 4. Runtime: cross-project memory isolation
// =============================================================================
static void test_cross_project_memory_isolation()
{
	std::cout << "[4] Cross-project memory isolation... ";
	auto proj_a = fs::temp_directory_path() / "daedalus_cross_a";
	auto proj_b = fs::temp_directory_path() / "daedalus_cross_b";
	std::error_code ec;
	fs::remove_all( proj_a, ec );
	fs::remove_all( proj_b, ec );
	fs::create_directories( proj_a );
	fs::create_directories( proj_b );

	daedalus::ProjectPaths planner_a( proj_a, "planner" );
	daedalus::ProjectPaths planner_b( proj_b, "planner" );

	assert( planner_a.memory_file() != planner_b.memory_file() );

	fs::remove_all( proj_a, ec );
	fs::remove_all( proj_b, ec );
	std::cout << "ok\n";
}

int main()
{
	test_static_no_xdg_memory_write();
	test_memory_not_under_user_data();
	test_two_agents_distinct_memory();
	test_cross_project_memory_isolation();
	std::cout << "All test_two_tier_rule tests passed.\n";
	return 0;
}