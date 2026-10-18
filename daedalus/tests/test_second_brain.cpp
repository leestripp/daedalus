// SPDX-License-Identifier: BSL-1.0
// tests/test_second_brain.cpp
//
// Unit tests for daedalus::SecondBrain (§SECOND-BRAIN, schema v10).
//
// What we pin:
//   B1.  Constructor creates the five PARA+inbox folders.
//   B2.  capture() writes <folder>/<slug>.md with frontmatter; read()
//        round-trips title/tags/body/id.
//   B3.  capture() with an empty title falls back to the body's first
//        heading; duplicate titles get -2, -3 … id suffixes.
//   B4.  update() changes fields, refreshes `updated`, preserves `created`.
//   B5.  append() adds markdown to the end of the body.
//   B6.  move() re-files the note; the old file is gone, read() follows.
//   B7.  remove() deletes file + index row; read() then fails.
//   B8.  list() filters by folder and by tag, newest-updated first.
//   B9.  search() finds notes by body text via FTS5 (porter stemming)
//        and returns snippets; junk FTS metacharacters cannot break it.
//   B10. [[wiki-links]] are extracted; backlinks() finds the referrers.
//   B11. reindex() picks up a file written by hand behind its back and
//        drops index rows for deleted files.
//   B12. Path-traversal ids (../, dots, slashes) never touch the fs.
//   B13. Lenient frontmatter: unknown keys ignored, missing keys default,
//        a file with no frontmatter at all still reads as a note.
//   B14. Schema v10: a v9 DB gains brain_notes + brain_fts on open.

// The default build is Release (-DNDEBUG), which would compile assert()
// away and make the whole file a no-op — force it back on (project
// pattern, cf. test_mcp_proxied_tool.cpp).
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <daedalus/settings.h>

#include "core/SecondBrain.h"

namespace fs = std::filesystem;
using namespace daedalus;

namespace {

struct TmpDir {
    fs::path path;
    explicit TmpDir(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("daedalus-brain-" + tag + "-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // anonymous namespace

int main() {
    TmpDir tmp("unit");
    auto store = SettingsStore::for_home(tmp.path);
    SecondBrain brain(tmp.path / "brain", store);

    // ── B1: folder tree exists ──
    for (const auto& f : {"inbox", "projects", "areas", "resources",
                          "archives"}) {
        assert(fs::is_directory(brain.root() / f));
    }

    // ── B2: capture + read round-trip ──
    auto n = brain.capture("resources", "Tiago Forte BASB",
                           {"pkm", "books"},
                           "# BASB\n\nCODE: Capture, Organize, Distill, "
                           "Express. See [[zettelkasten]].");
    assert(n.id == "tiago-forte-basb");
    assert(n.folder == "resources");
    assert(!n.created.empty() && n.created == n.updated);
    assert(fs::exists(brain.root() / "resources" / "tiago-forte-basb.md"));

    const auto text = slurp(brain.root() / "resources" /
                            "tiago-forte-basb.md");
    assert(text.rfind("---\n", 0) == 0);
    assert(text.find("title: \"Tiago Forte BASB\"") != std::string::npos);
    assert(text.find("tags: [pkm, books]") != std::string::npos);

    auto reread = brain.read("tiago-forte-basb");
    assert(reread.has_value());
    assert(reread->title == "Tiago Forte BASB");
    assert(reread->tags.size() == 2 && reread->tags[0] == "pkm");
    assert(reread->body.find("CODE: Capture") != std::string::npos);
    assert(reread->created == n.created);

    // ── B3: title fallback + id collision suffixes ──
    auto n2 = brain.capture("inbox", "", {},
                            "# Zettelkasten\n\nAtomic notes, one idea each.");
    assert(n2.id == "zettelkasten");
    assert(n2.title == "Zettelkasten");

    auto dup1 = brain.capture("inbox", "Zettelkasten", {}, "another one");
    auto dup2 = brain.capture("projects", "Zettelkasten", {}, "and again");
    assert(dup1.id == "zettelkasten-2");
    assert(dup2.id == "zettelkasten-3");

    // ── B4: update ──
    std::this_thread::sleep_for(std::chrono::seconds(1));  // updated differs
    const bool ok = brain.update("zettelkasten",
                                 std::string("Zettelkasten Method"),
                                 std::vector<std::string>{"pkm", "method"},
                                 std::string("# Zettelkasten Method\n\nrewritten"));
    assert(ok);
    auto upd = brain.read("zettelkasten");
    assert(upd->title == "Zettelkasten Method");
    assert(upd->tags.size() == 2 && upd->tags[1] == "method");
    assert(upd->body.find("rewritten") != std::string::npos);
    assert(upd->created == n2.created);          // preserved
    assert(upd->updated != upd->created);        // refreshed
    assert(!brain.update("no-such-note", std::string("x"), {}, {}));

    // ── B5: append ──
    assert(brain.append("zettelkasten", "More detail here."));
    auto app = brain.read("zettelkasten");
    assert(app->body.find("rewritten") != std::string::npos);
    assert(app->body.find("More detail here.") != std::string::npos);
    assert(!brain.append("no-such-note", "x"));

    // ── B6: move ──
    assert(brain.move("zettelkasten", "resources"));
    assert(!fs::exists(brain.root() / "inbox" / "zettelkasten.md"));
    assert(fs::exists(brain.root() / "resources" / "zettelkasten.md"));
    auto moved = brain.read("zettelkasten");
    assert(moved.has_value() && moved->folder == "resources");
    assert(!brain.move("no-such-note", "projects"));
    try {
        (void)brain.move("zettelkasten", "not-a-folder");
        assert(false && "move to invalid folder must throw");
    } catch (const std::invalid_argument&) {}

    // ── B7: remove ──
    assert(brain.remove("zettelkasten-3"));
    assert(!brain.read("zettelkasten-3").has_value());
    assert(!fs::exists(brain.root() / "projects" / "zettelkasten-3.md"));
    assert(!brain.remove("zettelkasten-3"));  // already gone

    // ── B8: list filters ──
    auto all = brain.list();
    assert(all.size() == 3);  // basb, zettelkasten, zettelkasten-2
    auto inbox_only = brain.list("inbox");
    assert(inbox_only.size() == 1 && inbox_only[0].id == "zettelkasten-2");
    auto pkm = brain.list("", "pkm");
    assert(pkm.size() == 2);  // basb + zettelkasten (not -2: untagged)
    try {
        (void)brain.list("not-a-folder");
        assert(false && "list on invalid folder must throw");
    } catch (const std::invalid_argument&) {}

    // ── B9: FTS5 search ──
    auto hits = brain.search("capture organize distill");
    assert(!hits.empty());
    assert(hits[0].note.id == "tiago-forte-basb");
    assert(!hits[0].snippet.empty());

    // Porter stemming: "organisations" should match "organize" → "organ".
    auto stem = brain.search("organizing");
    assert(!stem.empty() && stem[0].note.id == "tiago-forte-basb");

    // FTS metacharacters from the user must never reach MATCH raw.
    auto junk = brain.search("\" OR * NEAR( (");
    assert(junk.empty());  // sanitised to nothing → empty, no throw

    // ── B10: links + backlinks ──
    auto links = brain.read("tiago-forte-basb")->links;
    assert(links.size() == 1 && links[0] == "zettelkasten");
    auto back = brain.backlinks("zettelkasten");
    assert(back.size() == 1 && back[0] == "tiago-forte-basb");

    // ── B11: reindex picks up external edits + deletions ──
    write(brain.root() / "areas" / "hand-written.md",
          "---\ntitle: \"Hand Written\"\ntags: [manual]\n---\n\n"
          "Written with vim, not the API. Gobbledygook uniqueword.\n");
    auto re = brain.reindex();
    assert(re.indexed >= 1);
    assert(brain.read("hand-written").has_value());
    auto hand_hits = brain.search("uniqueword");
    assert(!hand_hits.empty() && hand_hits[0].note.id == "hand-written");

    // Delete the file behind the brain's back; reindex must drop the row.
    fs::remove(brain.root() / "areas" / "hand-written.md");
    re = brain.reindex();
    assert(re.removed >= 1);
    assert(brain.search("uniqueword").empty());
    assert(!brain.read("hand-written").has_value());

    // A no-change reindex is a no-op.
    re = brain.reindex();
    assert(re.indexed == 0 && re.removed == 0);

    // ── B12: path traversal never touches the filesystem ──
    assert(!brain.read("../../../etc/passwd").has_value());
    assert(!brain.read("..").has_value());
    assert(!brain.read("a/b").has_value());
    assert(!brain.remove("../../tmp/evil"));

    // ── B13: lenient frontmatter ──
    write(brain.root() / "inbox" / "messy.md",
          "---\ntitle: Messy Note\nunknown-key: whatever\n"
          "tags: [one, 'two']\nno-colon-line\n---\n\nBody here.\n");
    auto messy = brain.read("messy");
    assert(messy.has_value());
    assert(messy->title == "Messy Note");
    assert(messy->tags.size() == 2 && messy->tags[1] == "two");
    assert(messy->body == "Body here.");

    write(brain.root() / "inbox" / "bare.md",
          "# Just A Heading\n\nNo frontmatter at all.\n");
    auto bare = brain.read("bare");
    assert(bare.has_value());
    assert(bare->id == "bare");
    assert(bare->title == "Just A Heading");
    assert(bare->body.find("No frontmatter") != std::string::npos);

    // ── B14: v9 → v10 migration on an existing DB ──
    {
        TmpDir mig("migrate");
        const auto dbfile = mig.path / "daedalus.db";
        {
            // Create a store and force it back to schema v9 without the
            // brain tables, simulating a pre-v10 DB.
            auto s9 = SettingsStore::for_home(mig.path);
            (void)s9;
        }
        // Simulate the old DB: drop the brain tables, rewind the version.
        // (sqlite3 CLI is available on this project; use it for the hack.)
        const std::string cmd =
            "sqlite3 '" + dbfile.string() + "' "
            "\"DROP TABLE IF EXISTS brain_notes; "
            "DROP TABLE IF EXISTS brain_fts; "
            "DELETE FROM schema_version; "
            "INSERT INTO schema_version(version) VALUES (9);\"";
        assert(std::system(cmd.c_str()) == 0);

        // DbPool caches one connection per path and only reopens when the
        // file VANISHES — so "reopening" the same path would return the
        // stale pre-migration handle and ensure_schema would never run.
        // Copy the hacked DB to a fresh home instead: a new path forces a
        // real open, which runs the v9 → v10 migration.
        TmpDir reopen("migrate-reopen");
        fs::copy_file(dbfile, reopen.path / "daedalus.db");
        auto s10 = SettingsStore::for_home(reopen.path);  // open → migrate
        SettingsStore::BrainNoteRow row;
        row.id = "migrated-note";
        row.title = "Migrated";
        row.created = row.updated = "2026-10-18T00:00:00Z";
        s10.upsert_brain_note(row, "body after migration");
        auto got = s10.get_brain_note("migrated-note");
        assert(got.has_value() && got->title == "Migrated");
        assert(s10.search_brain_notes("migration*", 5).size() == 1);
        s10.delete_brain_note("migrated-note");
        assert(!s10.get_brain_note("migrated-note").has_value());
    }

    std::cout << "All SecondBrain tests passed (B1-B14)\n";
    return 0;
}
