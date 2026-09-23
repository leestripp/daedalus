// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/migrate.cpp
//
// Implementation of Plan.md Phase 5 — see migrate.h for the contract.
//
// Design notes:
//
//   * The "from" path defaults to $HOME/.daedalus (the LEGACY tier-1
//     location). We DON'T honour $DAEDALUS_HOME here because if the
//     user is migrating, they're specifically trying to move AWAY from
//     a hand-set single-root into XDG-compliant locations.
//
//   * The "to" paths come from `Paths::global()`. That means the user
//     can override the destination by setting $XDG_CONFIG_HOME /
//     $XDG_DATA_HOME before invoking the migration. Useful for
//     "migrate to a different machine" workflows:
//
//        XDG_DATA_HOME=/mnt/new-machine/share daedalus-cli --migrate-data-apply
//
//   * The mapping table is hard-coded for the eight known legacy
//     file/dir names from Plan.md §3.3.1. Adding a new file? Add it
//     here AND in Plan.md.

#include "migrate.h"

#include "daedalus/paths.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace daedalus_cli {

namespace {

/// Compute the default "from" path: $HOME/.daedalus.
fs::path default_legacy_root()
{
	const char* home = std::getenv("HOME");
	if (home && *home) {
		return fs::path(home) / ".daedalus";
	}
	return fs::path{};
}

/// Resolved XDG paths (re-evaluated each call so env-var overrides
/// during the run take effect — useful for tests).
struct XdgTargets
{
	fs::path config_root;
	fs::path data_root;
};
XdgTargets resolve_xdg_targets()
{
	// Use a fresh Paths instance so env-var overrides (e.g.
	// $XDG_CONFIG_HOME set just before --migrate-data) are honoured.
	// Paths::global() is a singleton and caches the first-resolved values.
	auto p = daedalus::Paths::make_default();
	return { p->user_config_root(), p->user_data_root() };
}

}  // namespace

MigratePlan compute_migration_plan( const MigrateOptions& opt )
{
	MigratePlan plan;
	plan.from_root = opt.from.empty() ? default_legacy_root() : opt.from;

	auto xdg = resolve_xdg_targets();
	plan.xdg_config_home = xdg.config_root;
	plan.xdg_data_home   = xdg.data_root;

	// Plan.md §3.3.1: GLOBAL — User state (XDG dirs).
	//   ~/.daedalus/settings/settings.json  →  $XDG_CONFIG_HOME/daedalus/config.json
	//   ~/.daedalus/mcp/servers.json        →  $XDG_CONFIG_HOME/daedalus/mcp/servers.json
	//   ~/.daedalus/mcp/.env                →  $XDG_CONFIG_HOME/daedalus/mcp/.env
	//   ~/.daedalus/skills/                 →  $XDG_DATA_HOME/daedalus/skills/
	struct Mapping
	{
		fs::path from;
		fs::path to;
	};
	std::vector<Mapping> mappings = {
		{ plan.from_root / "settings" / "settings.json",
		  xdg.config_root / "config.json" },
		{ plan.from_root / "mcp" / "servers.json",
		  xdg.config_root / "mcp" / "servers.json" },
		{ plan.from_root / "mcp" / ".env",
		  xdg.config_root / "mcp" / ".env" },
		{ plan.from_root / "skills",
		  xdg.data_root / "skills" },
	};

	plan.entries.reserve( mappings.size() );
	for ( const auto& m : mappings ) {
		MigratePlan::Entry e;
		e.from = m.from;
		e.to   = m.to;
		std::error_code ec;
		if ( fs::is_regular_file( m.from, ec ) ) {
			e.from_exists = true;
			e.size_bytes = fs::file_size( m.from, ec );
		} else if ( fs::is_directory( m.from, ec ) ) {
			e.from_exists = true;
			// Sum directory size for the plan — capped at 4 GiB to
			// avoid pathological walks.
			std::uintmax_t total = 0;
			for ( auto it = fs::recursive_directory_iterator(
			          m.from, ec );
			      !ec && it != fs::recursive_directory_iterator();
			      it.increment( ec ) ) {
				if ( it->is_regular_file( ec ) ) {
					total += it->file_size( ec );
					if ( total > ( 1ULL << 32 ) ) {
						total = ( 1ULL << 32 );
						break;
					}
				}
			}
			e.size_bytes = total;
		}
		plan.entries.push_back( std::move( e ) );
	}

	return plan;
}

void print_migration_plan( const MigratePlan& plan )
{
	std::cout << "Migration plan: " << plan.from_root << " → XDG dirs\n";
	std::cout << "  XDG_CONFIG_HOME = " << plan.xdg_config_home << "\n";
	std::cout << "  XDG_DATA_HOME   = " << plan.xdg_data_home << "\n\n";

	bool any = false;
	for ( const auto& e : plan.entries ) {
		if ( !e.from_exists ) continue;
		any = true;
		std::cout << "  " << e.from.string() << "\n";
		std::cout << "    → " << e.to.string() << "\n";
		std::cout << "    (" << e.size_bytes << " bytes)\n";
	}
	if ( !any ) {
		std::cout << "  (nothing to migrate — legacy layout empty or absent)\n";
	}
}

namespace {

/// Copy a file or directory tree (recursively) using std::filesystem::copy.
/// Returns true on success, false + diagnostic on failure.
bool copy_one_entry( const MigratePlan::Entry& e, bool verbose )
{
	std::error_code ec;

	// Decide: file copy or directory copy?
	if ( fs::is_regular_file( e.from, ec ) ) {
		// Ensure parent dir exists.
		fs::create_directories( e.to.parent_path(), ec );
		if ( ec ) {
			std::cerr << "ERROR: cannot create " << e.to.parent_path()
			          << ": " << ec.message() << "\n";
			return false;
		}
		if ( verbose ) {
			std::cout << "  COPY " << e.from << " → " << e.to << "\n";
		}
		fs::copy( e.from, e.to,
		          fs::copy_options::overwrite_existing, ec );
		if ( ec ) {
			std::cerr << "ERROR: copy " << e.from << " → " << e.to
			          << ": " << ec.message() << "\n";
			return false;
		}
		return true;
	}

	if ( fs::is_directory( e.from, ec ) ) {
		// Recursive copy. We use fs::copy with recursive + overwrite.
		// Existing destination subdirs are merged (newer source wins).
		fs::create_directories( e.to, ec );
		if ( ec && !fs::exists( e.to, ec ) ) {
			std::cerr << "ERROR: cannot create " << e.to
			          << ": " << ec.message() << "\n";
			return false;
		}
		if ( verbose ) {
			std::cout << "  COPY (recursive) " << e.from
			          << " → " << e.to << "\n";
		}
		fs::copy( e.from, e.to,
		          fs::copy_options::recursive
		              | fs::copy_options::overwrite_existing,
		          ec );
		if ( ec ) {
			std::cerr << "ERROR: recursive copy " << e.from
			          << " → " << e.to << ": " << ec.message() << "\n";
			return false;
		}
		return true;
	}

	// From path doesn't exist anymore (raced with a concurrent edit).
	// That's a soft failure — we count it as a no-op.
	if ( verbose ) {
		std::cout << "  SKIP " << e.from << " (no longer exists)\n";
	}
	return true;
}

}  // namespace

int apply_migration_plan( const MigratePlan& plan, bool verbose )
{
	int copied = 0;
	for ( const auto& e : plan.entries ) {
		if ( !e.from_exists ) continue;
		if ( !copy_one_entry( e, verbose ) ) return -1;
		++copied;
	}
	return copied;
}

int run_migrate_data( const MigrateOptions& opt )
{
	auto plan = compute_migration_plan( opt );

	print_migration_plan( plan );

	if ( opt.dry_run ) {
		std::cout << "\nDRY RUN — re-run with --migrate-data-apply to copy files.\n";
		std::cout << "            (The legacy " << plan.from_root
		          << " directory is NEVER removed automatically.\n";
		std::cout << "             After verifying the new location works, "
		             "manually `rm -rf` it.)\n";
		return 0;
	}

	std::cout << "\nApplying migration...\n";
	int rc = apply_migration_plan( plan, opt.verbose );
	if ( rc < 0 ) {
		std::cerr << "Migration failed.\n";
		return 2;
	}
	std::cout << "Migrated " << rc << " entries. Verify with:\n";
	std::cout << "  ls -la " << plan.xdg_config_home << "\n";
	std::cout << "  ls -la " << plan.xdg_data_home << "/skills/\n";
	std::cout << "Then manually remove the legacy directory:\n";
	std::cout << "  rm -rf " << plan.from_root << "\n";
	return rc == 0 ? 1 : 0;  // exit 1 = "nothing to migrate"
}

MigrateArgResult detect_migrate_data_args( int argc, char* argv[] )
{
	MigrateArgResult r;
	for ( int i = 1; i < argc; ++i ) {
		std::string a = argv[i];
		if ( a == "--migrate-data" ) {
			r.present = true;
			r.options.dry_run = true;
			r.consumed_args.push_back( a );
		} else if ( a == "--migrate-data-apply" ) {
			r.present = true;
			r.options.dry_run = false;
			r.consumed_args.push_back( a );
		} else if ( a == "--migrate-data-from" && i + 1 < argc ) {
			r.present = true;
			r.options.from = argv[++i];
			r.consumed_args.push_back( a );
			r.consumed_args.push_back( argv[i] );
		} else if ( a == "--migrate-data-verbose" ) {
			r.options.verbose = true;
			r.consumed_args.push_back( a );
		}
	}
	return r;
}

}  // namespace daedalus_cli