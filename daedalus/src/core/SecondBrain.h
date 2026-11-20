// SPDX-License-Identifier: BSL-1.0
// src/core/SecondBrain.h
//
// §SECOND-BRAIN: the Global Second Brain — a plain-markdown knowledge base
// shared by every agent on this machine.
//
// Design (see docs/storage.md):
//   - Source of truth: markdown files under second_brain_directory (default
//     <user_data_root>/brain/),
//     organised in an inbox + PARA folders (projects / areas / resources /
//     archives). Human-readable and editable with any editor — the agent
//     runtime owns nothing you cannot open in vim.
//   - One idea per note (Zettelkasten atomicity). YAML frontmatter carries
//     id/title/tags/created/updated; [[wiki-links]] connect notes.
//   - Filename = note id = unique slug across all folders, so [[links]]
//     resolve without a folder prefix.
//   - SQLite FTS5 (schema v10 tables brain_notes + brain_fts in the global
//     SettingsStore DB) is a REBUILDABLE CACHE over the files — never a
//     second source of truth. brain_reindex rebuilds it from disk.
//
// GLOBAL by design: SecondBrain::global() uses the directory persisted in the
// global SettingsStore, regardless of any agent's per-project daedalus_home.
// The default is <user_data_root>/brain; hosts can select another absolute
// directory. Changing it selects a different knowledge base (no implicit move)
// and takes effect on the next brain operation.
//
// Thread/process safety: public operations serialise on an internal mutex and
// a bounded flock(2) at <brain_root>/.lock. Writers hold it exclusively;
// readers share it. Markdown replacement is atomic and the SQLite index is
// transactional, so independent agents cannot overwrite each other's edits.

#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "daedalus/settings.h"   // SettingsStore::BrainNoteRow / BrainSearchHit

namespace daedalus {

/// One note as hosts/tools see it: frontmatter metadata + markdown body.
struct BrainNote
{
    std::string id;                    // slug, unique across folders
    std::string folder = "inbox";      // inbox|projects|areas|resources|archives
    std::string title;
    std::vector<std::string> tags;
    std::string created;               // ISO-8601 UTC
    std::string updated;               // ISO-8601 UTC
    std::vector<std::string> links;    // [[wiki-link]] targets found in body
    std::string body;                  // markdown WITHOUT the frontmatter block
};

/// Summary row for list/search results (no body).
using BrainNoteMeta = SettingsStore::BrainNoteRow;
using BrainSearchHit = SettingsStore::BrainSearchHit;

class SecondBrain
{
public:
    /// Construct over an explicit brain directory + store (tests use this
    /// with a temp dir + SettingsStore::for_home). Creates the folder tree.
    SecondBrain(std::filesystem::path brain_root, SettingsStore store);

    /// Process-wide entry point over SettingsStore::instance(). Uses the
    /// persisted second_brain_directory (default <user_data_root>/brain),
    /// creating and indexing that root on first selection. Instances for old
    /// roots remain alive so concurrent operations can finish safely.
    static SecondBrain& global();

    /// The five folders, in display order. inbox first.
    static const std::vector<std::string>& folders();

    static bool is_valid_folder(std::string_view folder);

    // --- Capture / read / update -------------------------------------------

    /// Create a note. `folder` must be one of folders(); `title` may be
    /// empty (falls back to the body's first heading, then "untitled").
    /// The id is a slug of the title; on collision a numeric suffix
    /// (-2, -3, …) keeps ids unique. Writes the file and indexes it.
    /// Returns the full note.
    BrainNote capture(std::string_view folder,
                      std::string_view title,
                      const std::vector<std::string>& tags,
                      std::string_view body);

    /// Read one note by id (any folder). nullopt if no file matches.
    std::optional<BrainNote> read(std::string_view id) const;

    /// Replace title/tags/body of an existing note (empty optional = keep
    /// current value). `updated` is refreshed, `created` preserved.
    /// Returns false if the id does not exist.
    bool update(std::string_view id,
                const std::optional<std::string>& title,
                const std::optional<std::vector<std::string>>& tags,
                const std::optional<std::string>& body);

    /// Append markdown to the end of the note body (CODE "capture more
    /// into an existing note" workflow). Returns false if id missing.
    bool append(std::string_view id, std::string_view markdown);

    /// Delete the file + index row. Returns false if id missing.
    bool remove(std::string_view id);

    // --- Organise -----------------------------------------------------------

    /// Move a note to another folder (PARA re-filing). Updates the index.
    /// Returns false if id missing or folder invalid.
    bool move(std::string_view id, std::string_view new_folder);

    /// Notes linking TO `id` (backlinks) — scans every note's links.
    std::vector<std::string> backlinks(std::string_view id) const;

    // --- Find ---------------------------------------------------------------

    /// List notes (metadata only), newest-updated first. Empty folder =
    /// all folders; empty tag = no tag filter.
    std::vector<BrainNoteMeta> list(std::string_view folder = {},
                                    std::string_view tag = {},
                                    int limit = 100) const;

    /// Full-text search over title/tags/body. Plain-language query; terms
    /// are FTS5-sanitised internally (AND semantics, prefix matching).
    std::vector<BrainSearchHit> search(std::string_view query,
                                       int limit = 20) const;

    // --- Index maintenance ---------------------------------------------------

    /// Rebuild the index from disk: upsert every file whose mtime/size
    /// differs from the stored row, delete index rows whose files vanished.
    /// Returns {indexed, removed}. Cheap when nothing changed (mtime diff).
    struct ReindexResult { int indexed = 0; int removed = 0; };
    ReindexResult reindex();

    // --- Paths ----------------------------------------------------------------

    const std::filesystem::path& root() const { return root_; }
    std::filesystem::path folder_path(std::string_view folder) const;
    std::filesystem::path note_path(std::string_view folder,
                                    std::string_view id) const;

private:
    /// Locate the file for `id` in any folder. Empty path if not found.
    std::filesystem::path find_note_file(std::string_view id) const;

    /// Read without taking the process lock. The public reader holds the
    /// shared lock; mutations already hold the exclusive lock.
    std::optional<BrainNote> read_unlocked(std::string_view id) const;

    /// Write note (frontmatter + body) to disk and upsert the index row.
    /// Caller holds mu_ and the exclusive inter-process lock.
    void write_and_index(const BrainNote& note);

    /// Parse one markdown file into a BrainNote. Frontmatter is parsed
    /// leniently (missing keys default; unknown keys ignored).
    static std::optional<BrainNote> parse_file(
        const std::filesystem::path& path, std::string_view folder);

    /// Extract [[wiki-link]] targets from a markdown body.
    static std::vector<std::string> extract_links(std::string_view body);

    /// Render frontmatter + body back to the canonical file text.
    static std::string render_file(const BrainNote& note);

    /// Turn an arbitrary title into a filesystem-safe slug.
    static std::string slugify(std::string_view title);

    /// Convert an arbitrary user query into a safe FTS5 MATCH expression:
    /// alnum runs become "term"* prefix tokens joined by AND.
    static std::string sanitise_fts_query(std::string_view query);

    /// ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ" for now (gmtime_r — no TZ state).
    static std::string now_iso8601();

    std::filesystem::path root_;
    SettingsStore store_;
    mutable std::recursive_mutex mu_;
};

}  // namespace daedalus
