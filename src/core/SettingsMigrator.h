// SPDX-License-Identifier: BSL-1.0
// src/core/SettingsMigrator.h
//
// One-shot JSON → SQLite settings migrator (v0.3.1).
//
// See SettingsMigrator.cpp for the full policy.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace daedalus {

class SettingsStore;  // forward decl

struct MigrateReport
{
    int items_migrated = 0;            // total rows/keys written to the store
    std::vector<std::filesystem::path> files_touched;  // JSON files consumed
    bool skipped = false;
    std::string reason;                // human-readable; for the one-time log
};

class SettingsMigrator
{
public:
    /// True if any legacy JSON file exists AND the SQLite store is empty.
    /// Cheap — just stat()s the candidate paths. Used by SettingsManager
    /// to decide whether to auto-migrate on load().
    ///
    /// Pass an explicit `config_json_path` (and optionally
    /// `mcp_servers_path`) to override the defaults — used by tests and
    /// by the `--migrate-data --from <path>` machinery.
    static bool needs_migration(const SettingsStore& store,
                                const std::filesystem::path& config_json_path  = {},
                                const std::filesystem::path& mcp_servers_path = {});

    /// Run the migration. Writes to `store` (caller's responsibility to
    /// own the lifetime). If `mark_files` is true, renames each migrated
    /// JSON file to `<name>.migrated` so the user can see what changed
    /// without losing the old data.
    static MigrateReport run(SettingsStore& store,
                             const std::filesystem::path& config_json_path  = {},
                             const std::filesystem::path& mcp_servers_path = {},
                             bool mark_files = true);
};

}  // namespace daedalus