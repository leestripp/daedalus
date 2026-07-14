/**
 * @file daedalus/default_paths.h
 * @brief Default filesystem locations matching the historical CLI layout.
 *
 * === Per-project data (the only tier most users care about) ===
 *
 * Each Agent instance working on a project gets its own per-project
 * namespace, namespaced by `agent_name`:
 *
 *     agent_name == ""    ->  <project_root>/.daedalus/
 *     agent_name == "bob" ->  <project_root>/.daedalus-bob/
 *
 * The empty string is the backward-compat default. Multiple Agent
 * instances on the same project (planner / actor / verifier / coder,
 * spawned by daedalus-manager or daedalus-subagent) each get their own
 * subdir so memories never leak between agents on the same project.
 *
 * The legacy directory names `.daedalus/` and `.daedalus-<name>/` (from
 * the pre-rename code) are still honoured as a read fallback for one
 * release cycle so existing projects keep working. NEW writes always go
 * to the new `.daedalus*` path. See Plan.md §1.1 and use
 * `--migrate-data` for the explicit one-shot rename.
 *
 * === Per-user (global) data ===
 *
 * These helpers exist purely so hosts can call:
 *
 *   AgentConfig cfg;
 *   cfg.settings_path    = daedalus::default_settings_path();
 *   cfg.mcp_servers_path = daedalus::default_mcp_servers_path();
 *
 * without hardcoding `~/.daedalus/...` in their own code.  Each helper
 * honours $DAEDALUS_HOME (override for sandboxed / test runs). This
 * global tier is going XDG-compliant in Phase 13 — see Plan.md.
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace daedalus {

/// $HOME/.daedalus (or $DAEDALUS_HOME, or $DAEDALUSLIB_HOME for testing)
std::filesystem::path default_daedalus_home();

/// ~/.daedalus/settings/settings.json
std::filesystem::path default_settings_path();

/// ~/.daedalus/mcp/servers.json
std::filesystem::path default_mcp_servers_path();

/// ~/.daedalus/mcp/.env
std::filesystem::path default_mcp_env_path();

/// @brief Validate an agent_name string (§Phase 8.8).
///
/// Returns true if the name matches `^[A-Za-z0-9_.-]{1,64}$` AND
/// does not start with '.' or '-' (which would respectively
/// denote hidden directories or CLI flags). Empty string is
/// permitted — it means "no namespace, use the project default".
///
/// Rationale for each reject case lives in Agent.md §Phase 8.8
/// (Validation rules table).
[[nodiscard]] bool is_valid_agent_name(std::string_view name) noexcept;

/// @brief Resolve the per-agent runtime-state directory (§Phase 8.8).
///
/// Returns `<project_root>/.daedalus/` (empty agent_name) or
/// `<project_root>/.daedalus-<name>/` (named agent_name).
///
/// §Phase 12 (rename): the legacy `.daedalus[-<name>]` paths are STILL
/// honoured as a read-only fallback for one release cycle — if the new
/// `.daedalus*` path doesn't exist but the old `.daedalus*` one does, the
/// old one is returned. NEW writes always create the new path.
///
/// The directory is NOT auto-created — callers do that on first
/// write. Callers MUST pass an already-validated name (use
/// is_valid_agent_name() first, or let Agent::initialize()
/// validate-then-throw).
[[nodiscard]] std::filesystem::path resolve_agent_dir(
    const std::filesystem::path& project_root,
    std::string_view agent_name);

}  // namespace daedalus
