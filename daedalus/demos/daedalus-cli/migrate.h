// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/migrate.h
//
// Plan.md Phase 5: `daedalus-cli --migrate-data` one-shot CLI that
// copies legacy `~/.daedalus/` user-tier state into the XDG-compliant
// locations resolved by `Paths::global()`.
//
// The legacy directory is NEVER modified or removed — this is a
// copy-then-verify-then-manually-rm design. After migration:
//   1. Run the agent normally.
//   2. Verify it sees the migrated state (settings.json, MCP servers,
//      skill packs).
//   3. Manually `rm -rf ~/.daedalus` once you trust the new location.
//
// This module is part of the CLI host (demos/daedalus-cli/), not
// the library — it can freely use stdio (the no-stdio rule is for
// libdaedalus.a only, see Agent.md §Phase 5.1).
#pragma once

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace daedalus_cli {

struct MigrateOptions
{
	std::filesystem::path from;        ///< Legacy root (default: $HOME/.daedalus)
	std::filesystem::path project_root; ///< Unused today; reserved for per-project migration
	bool                 dry_run = true;   ///< Default to dry-run for safety
	bool                 verbose = false;  ///< Print every file copy
};

struct MigratePlan
{
	struct Entry
	{
		std::filesystem::path from;
		std::filesystem::path to;
		std::uintmax_t        size_bytes = 0;
		bool                  from_exists = false;
	};

	std::vector<Entry>   entries;
	std::filesystem::path from_root;
	std::filesystem::path xdg_config_home;
	std::filesystem::path xdg_data_home;
};

/// Compute the migration plan WITHOUT touching the filesystem (other than
/// `stat`-style probes to determine which files exist).
MigratePlan compute_migration_plan( const MigrateOptions& opt );

/// Print a plan to stdout in human-readable form. Safe to call on an
/// empty plan.
void print_migration_plan( const MigratePlan& plan );

/// Apply the plan — copies each existing source to its destination.
/// Creates parent directories as needed. Returns the number of files
/// successfully copied. Stops and returns -1 on the first I/O error
/// (after printing what happened).
int apply_migration_plan( const MigratePlan& plan, bool verbose );

/// Top-level entry point invoked from main(). Returns process exit
/// code (0 = success, 1 = no-op, 2 = error).
int run_migrate_data( const MigrateOptions& opt );

/// Detect the `--migrate-data` / `--migrate-data-apply` /
/// `--migrate-data-from DIR` argv triple. Mutates `argc`/`argv` is
/// NOT done — the caller is expected to short-circuit before normal
/// CLI parsing if this returns `true`.
struct MigrateArgResult
{
	bool                 present = false;
	MigrateOptions       options;
	std::vector<std::string> consumed_args; ///< for diagnostic echo
};
MigrateArgResult detect_migrate_data_args( int argc, char* argv[] );

}  // namespace daedalus_cli