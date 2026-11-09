// SPDX-License-Identifier: BSL-1.0
// tests/test_second_brain_concurrency.cpp
//
// Multi-process and crash-consistency regression coverage for the Global
// Second Brain. Every test uses isolated temporary paths; the real user DB
// and markdown tree are never opened.

#undef NDEBUG
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sqlite3.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <daedalus/settings.h>

#include "core/InterprocessLock.h"
#include "core/SecondBrain.h"

namespace fs = std::filesystem;
using namespace daedalus;

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct TmpDir
{
    fs::path path;

    explicit TmpDir(const std::string& tag)
    {
        static uint64_t sequence = 0;
        path = fs::temp_directory_path() /
               ("daedalus-brain-hardening-" + tag + "-" +
                std::to_string(static_cast<long long>(::getpid())) + "-" +
                std::to_string(++sequence));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TmpDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

template <typename Fn>
pid_t spawn(Fn&& fn)
{
    const pid_t pid = ::fork();
    require(pid >= 0, std::string("fork failed: ") + std::strerror(errno));
    if (pid == 0) {
        int status = 0;
        try {
            fn();
        } catch (const std::exception& e) {
            std::cerr << "child " << ::getpid() << ": " << e.what() << '\n';
            status = 1;
        } catch (...) {
            std::cerr << "child " << ::getpid() << ": unknown exception\n";
            status = 2;
        }
        std::cerr.flush();
        ::_exit(status);
    }
    return pid;
}

int wait_raw(const pid_t pid)
{
    int status = 0;
    for (;;) {
        const auto result = ::waitpid(pid, &status, 0);
        if (result == pid) return status;
        require(result < 0 && errno == EINTR, "waitpid failed");
    }
}

void wait_ok(const pid_t pid)
{
    const int status = wait_raw(pid);
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
            "worker failed (wait status " + std::to_string(status) + ")");
}


void write_text(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "cannot write test fixture " + path.string());
    out << text;
    out.close();
    require(static_cast<bool>(out), "cannot finish test fixture " + path.string());
}

size_t line_occurrences(std::string_view text, std::string_view needle)
{
    size_t count = 0;
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string_view::npos) {
        const auto end = pos + needle.size();
        if ((pos == 0 || text[pos - 1] == '\n') &&
            (end == text.size() || text[end] == '\n'))
            ++count;
        pos = end;
    }
    return count;
}

class SqliteDb
{
public:
    explicit SqliteDb(const fs::path& path)
    {
        const int rc = sqlite3_open(path.c_str(), &db_);
        require(rc == SQLITE_OK, "sqlite3_open failed for " + path.string());
        sqlite3_busy_timeout(db_, 5000);
    }

    ~SqliteDb() { if (db_) sqlite3_close(db_); }

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    void exec(const std::string& sql)
    {
        char* error = nullptr;
        const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
        const std::string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        require(rc == SQLITE_OK, "SQLite exec failed: " + message);
    }

    std::optional<std::string> one(const std::string& sql)
    {
        sqlite3_stmt* stmt = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
        require(rc == SQLITE_OK,
                std::string("SQLite prepare failed: ") + sqlite3_errmsg(db_));
        const int step = sqlite3_step(stmt);
        std::optional<std::string> result;
        if (step == SQLITE_ROW) {
            const auto* value = sqlite3_column_text(stmt, 0);
            result = value ? reinterpret_cast<const char*>(value) : "";
        } else {
            require(step == SQLITE_DONE,
                    std::string("SQLite query failed: ") + sqlite3_errmsg(db_));
        }
        sqlite3_finalize(stmt);
        return result;
    }

private:
    sqlite3* db_ = nullptr;
};

void test_lock_modes_and_timeout()
{
    TmpDir tmp("lock");
    const auto path = tmp.path / ".lock";
    {
        InterprocessFileLock writer(path, InterprocessLockMode::exclusive);
        require(writer.acquired(), "exclusive lock was not acquired");
        InterprocessFileLock blocked(path, InterprocessLockMode::shared,
                                     std::chrono::milliseconds(40));
        require(!blocked.acquired(), "shared lock bypassed an exclusive holder");
        require(blocked.error().find("timed out") != std::string::npos,
                "lock failure did not explain its timeout");
    }
    InterprocessFileLock reader1(path, InterprocessLockMode::shared);
    InterprocessFileLock reader2(path, InterprocessLockMode::shared);
    require(reader1.acquired() && reader2.acquired(),
            "shared locks did not coexist");
    InterprocessFileLock writer(path, InterprocessLockMode::exclusive,
                                std::chrono::milliseconds(40));
    require(!writer.acquired(), "exclusive lock bypassed shared holders");
}

void test_process_captures()
{
    TmpDir tmp("captures");
    const auto root = tmp.path / "brain";
    const auto home = tmp.path / "home";

    std::vector<pid_t> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.push_back(spawn([=] {
            SecondBrain brain(root, SettingsStore::for_home(home));
            for (int i = 0; i < 10; ++i) {
                const auto title = "worker-" + std::to_string(worker) +
                                   "-note-" + std::to_string(i);
                (void)brain.capture("inbox", title, {"stress"},
                                    "unique capture body " + title);
            }
        }));
    }
    for (const auto pid : workers) wait_ok(pid);

    workers.clear();
    for (int worker = 0; worker < 4; ++worker) {
        workers.push_back(spawn([=] {
            SecondBrain brain(root, SettingsStore::for_home(home));
            for (int i = 0; i < 10; ++i) {
                (void)brain.capture("projects", "shared-title", {"stress"},
                                    "same-title-" + std::to_string(worker) +
                                    "-" + std::to_string(i));
            }
        }));
    }
    for (const auto pid : workers) wait_ok(pid);

    SecondBrain brain(root, SettingsStore::for_home(home));
    const auto rows = brain.list("", "", 200);
    require(rows.size() == 80, "expected 80 captures, got " +
                                std::to_string(rows.size()));
    std::set<std::string> ids;
    size_t same_title = 0;
    for (const auto& row : rows) {
        ids.insert(row.id);
        if (row.id.rfind("shared-title", 0) == 0) ++same_title;
        require(brain.read(row.id).has_value(), "captured note is unreadable");
    }
    require(ids.size() == rows.size(), "capture ids were overwritten");
    require(same_title == 40, "same-title suffix allocation lost notes");
}

void test_concurrent_appends_and_readers()
{
    TmpDir tmp("appends");
    const auto root = tmp.path / "brain";
    const auto home = tmp.path / "home";

    wait_ok(spawn([=] {
        SecondBrain brain(root, SettingsStore::for_home(home));
        (void)brain.capture("inbox", "shared-append", {}, "seed-marker");
    }));

    std::vector<pid_t> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.push_back(spawn([=] {
            SecondBrain brain(root, SettingsStore::for_home(home));
            for (int i = 0; i < 12; ++i) {
                require(brain.append("shared-append",
                                     "APPEND-" + std::to_string(worker) + "-" +
                                     std::to_string(i)),
                        "append could not find shared note");
            }
        }));
    }
    for (int reader = 0; reader < 2; ++reader) {
        workers.push_back(spawn([=] {
            SecondBrain brain(root, SettingsStore::for_home(home));
            for (int i = 0; i < 40; ++i) {
                const auto note = brain.read("shared-append");
                require(note.has_value() &&
                            note->body.find("seed-marker") != std::string::npos,
                        "reader observed a missing or partial note");
                require(brain.search("seed-marker", 5).size() == 1,
                        "reader observed a partially updated index");
                require(brain.list("inbox", "", 5).size() == 1,
                        "reader observed duplicate list rows");
            }
        }));
    }
    for (const auto pid : workers) wait_ok(pid);

    SecondBrain brain(root, SettingsStore::for_home(home));
    const auto note = brain.read("shared-append");
    require(note.has_value(), "appended note vanished");
    for (int worker = 0; worker < 4; ++worker) {
        for (int i = 0; i < 12; ++i) {
            const auto marker = "APPEND-" + std::to_string(worker) + "-" +
                                std::to_string(i);
            require(line_occurrences(note->body, marker) == 1,
                    "append marker missing or duplicated: " + marker);
        }
    }
    const auto reindexed = brain.reindex();
    require(reindexed.indexed == 0 && reindexed.removed == 0,
            "successful appends were not fully indexed");
}

void test_mixed_mutations_and_reindex()
{
    TmpDir tmp("mixed");
    const auto root = tmp.path / "brain";
    const auto home = tmp.path / "home";

    wait_ok(spawn([=] {
        SecondBrain brain(root, SettingsStore::for_home(home));
        (void)brain.capture("inbox", "mixed-hub", {}, "backlink target");
        for (int i = 0; i < 24; ++i) {
            (void)brain.capture("inbox", "mixed-" + std::to_string(i), {},
                                "see [[mixed-hub]]");
        }
    }));

    std::vector<pid_t> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.push_back(spawn([=] {
            SecondBrain brain(root, SettingsStore::for_home(home));
            for (int n = 0; n < 6; ++n) {
                const int i = worker * 6 + n;
                const auto id = "mixed-" + std::to_string(i);
                require(brain.move(id, "projects"), "mixed move to projects failed");
                require(brain.update(id, std::nullopt,
                                     std::vector<std::string>{"mixed", "worker"},
                                     std::nullopt),
                        "mixed update failed");
                require(brain.append(id, "MUTATION-" + std::to_string(i)),
                        "mixed append failed");
                require(brain.move(id, "archives"), "mixed move to archives failed");
                if (i % 3 == 0) require(brain.remove(id), "mixed remove failed");
            }
        }));
    }
    workers.push_back(spawn([=] {
        SecondBrain brain(root, SettingsStore::for_home(home));
        for (int i = 0; i < 12; ++i) (void)brain.reindex();
    }));
    workers.push_back(spawn([=] {
        SecondBrain brain(root, SettingsStore::for_home(home));
        for (int i = 0; i < 30; ++i) {
            const auto links = brain.backlinks("mixed-hub");
            require(links.size() <= 24, "backlink scan returned duplicates");
            const auto rows = brain.list("", "", 100);
            std::set<std::string> ids;
            for (const auto& row : rows) ids.insert(row.id);
            require(ids.size() == rows.size(), "mixed list returned duplicate ids");
            (void)brain.search("backlink", 50);
        }
    }));
    for (const auto pid : workers) wait_ok(pid);

    SecondBrain brain(root, SettingsStore::for_home(home));
    const auto rows = brain.list("", "", 100);
    require(rows.size() == 17, "mixed mutations produced the wrong survivor count");
    for (const auto& row : rows) {
        if (row.id == "mixed-hub") continue;
        require(row.folder == "archives", "survivor did not finish its moves");
        const auto note = brain.read(row.id);
        require(note.has_value(), "mixed survivor is unreadable");
        const auto number = row.id.substr(std::string("mixed-").size());
        require(line_occurrences(note->body, "MUTATION-" + number) == 1,
                "mixed update/append was lost");
    }
    require(brain.backlinks("mixed-hub").size() == 16,
            "final backlinks do not match surviving notes");
    const auto settled = brain.reindex();
    require(settled.indexed == 0 && settled.removed == 0,
            "mixed mutation index did not settle");
}

void test_wal_and_index_transactions()
{
    TmpDir tmp("transactions");
    const auto root = tmp.path / "brain";
    const auto home = tmp.path / "home";
    SecondBrain brain(root, SettingsStore::for_home(home));
    (void)brain.capture("inbox", "tx-note", {}, "original-body");

    SqliteDb db(home / "daedalus.db");
    require(db.one("PRAGMA journal_mode") == std::optional<std::string>("wal"),
            "SettingsStore did not enable persistent WAL mode");
    // SQLite cannot place triggers on virtual tables. Substitute a regular
    // table with the same columns so the insert fails AFTER the metadata
    // update and FTS delete, which is the atomicity boundary under test.
    db.exec("DROP TABLE brain_fts");
    db.exec("CREATE TABLE brain_fts(id TEXT, title TEXT, tags TEXT, body TEXT)");
    db.exec("INSERT INTO brain_fts VALUES('tx-note', 'tx-note', '', "
            "'original-body')");
    db.exec("CREATE TRIGGER fail_brain_fts_insert "
            "BEFORE INSERT ON brain_fts WHEN NEW.id = 'tx-note' "
            "BEGIN SELECT RAISE(FAIL, 'forced FTS insert failure'); END");

    bool failed = false;
    try {
        (void)brain.update("tx-note", std::string("replacement-title"),
                           std::nullopt, std::string("replacement-body"));
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "forced FTS failure did not reach the caller");
    require(db.one("SELECT title FROM brain_notes WHERE id = 'tx-note'") ==
                std::optional<std::string>("tx-note"),
            "metadata changed despite FTS rollback");
    require(db.one("SELECT body FROM brain_fts WHERE id = 'tx-note'") ==
                std::optional<std::string>("original-body"),
            "FTS row changed despite rollback");

    db.exec("DROP TRIGGER fail_brain_fts_insert");
    db.exec("DROP TABLE brain_fts");
    db.exec("CREATE VIRTUAL TABLE brain_fts USING fts5("
            "id UNINDEXED, title, tags, body, "
            "tokenize = 'porter unicode61')");
    const auto repaired = brain.reindex();
    require(repaired.indexed == 1, "reindex did not repair the replaced file");
    require(brain.search("replacement-body", 5).size() == 1,
            "repaired index does not contain replacement body");

    db.exec("CREATE TRIGGER fail_brain_note_delete "
            "BEFORE DELETE ON brain_notes WHEN OLD.id = 'tx-note' "
            "BEGIN SELECT RAISE(FAIL, 'forced metadata delete failure'); END");
    failed = false;
    try {
        (void)brain.remove("tx-note");
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "forced metadata delete failure did not reach the caller");
    require(db.one("SELECT COUNT(*) FROM brain_notes WHERE id = 'tx-note'") ==
                std::optional<std::string>("1"),
            "metadata delete unexpectedly succeeded");
    require(db.one("SELECT COUNT(*) FROM brain_fts WHERE id = 'tx-note'") ==
                std::optional<std::string>("1"),
            "FTS delete was not rolled back with metadata delete");
    db.exec("DROP TRIGGER fail_brain_note_delete");
    const auto removed = brain.reindex();
    require(removed.removed == 1, "reindex did not reconcile deleted file");
}

void test_reindex_rolls_back_as_one_transaction()
{
    TmpDir tmp("reindex");
    const auto root = tmp.path / "brain";
    const auto home = tmp.path / "home";
    SecondBrain brain(root, SettingsStore::for_home(home));
    write_text(root / "inbox" / "a.md", "# A\n\nalpha-body\n");
    write_text(root / "inbox" / "b.md", "# B\n\nbeta-body\n");

    SqliteDb db(home / "daedalus.db");
    db.exec("DROP TABLE brain_fts");
    db.exec("CREATE TABLE brain_fts(id TEXT, title TEXT, tags TEXT, body TEXT)");
    db.exec("CREATE TRIGGER fail_reindex_b "
            "BEFORE INSERT ON brain_fts WHEN NEW.id = 'b' "
            "BEGIN SELECT RAISE(FAIL, 'forced reindex failure'); END");
    bool failed = false;
    try {
        (void)brain.reindex();
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed, "forced reindex failure did not reach the caller");
    require(db.one("SELECT COUNT(*) FROM brain_notes") ==
                std::optional<std::string>("0"),
            "reindex committed a prefix of its metadata updates");
    require(db.one("SELECT COUNT(*) FROM brain_fts") ==
                std::optional<std::string>("0"),
            "reindex committed a prefix of its FTS updates");
    db.exec("DROP TRIGGER fail_reindex_b");
    db.exec("DROP TABLE brain_fts");
    db.exec("CREATE VIRTUAL TABLE brain_fts USING fts5("
            "id UNINDEXED, title, tags, body, "
            "tokenize = 'porter unicode61')");
    const auto result = brain.reindex();
    require(result.indexed == 2, "complete reindex did not index both files");
}

size_t markdown_file_count(const fs::path& root)
{
    size_t count = 0;
    for (const auto& folder : SecondBrain::folders()) {
        std::error_code ec;
        if (!fs::is_directory(root / folder, ec)) continue;
        for (const auto& entry : fs::directory_iterator(root / folder, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".md")
                ++count;
        }
    }
    return count;
}

void test_sigkill_reopen_recovery()
{
    TmpDir tmp("crash");
    const auto root = tmp.path / "brain";
    const auto home = tmp.path / "home";
    const std::string payload(8192, 'x');

    const pid_t writer = spawn([=] {
        SecondBrain brain(root, SettingsStore::for_home(home));
        for (int i = 0; i < 2000; ++i) {
            (void)brain.capture("inbox", "crash-note-" + std::to_string(i), {},
                                "crash-payload-" + std::to_string(i) + "\n" +
                                payload);
        }
    });

    bool killed = false;
    for (int attempt = 0; attempt < 5000 && !killed; ++attempt) {
        if (markdown_file_count(root) >= 4) {
            require(::kill(writer, SIGKILL) == 0, "SIGKILL failed");
            killed = true;
            break;
        }
        int status = 0;
        const auto result = ::waitpid(writer, &status, WNOHANG);
        require(result == 0, "crash writer exited before it could be killed");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(killed, "timed out waiting to kill the writer");
    const int status = wait_raw(writer);
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
            "writer did not terminate from SIGKILL");

    // A new store/brain exercises WAL recovery and the kernel's automatic
    // release of the killed process's flock. Every renamed destination must
    // be a complete note; an uncommitted temp file is ignored by reindex.
    SecondBrain brain(root, SettingsStore::for_home(home));
    const auto files = markdown_file_count(root);
    require(files >= 4, "no completed notes survived the crash");
    (void)brain.reindex();
    const auto rows = brain.list("", "", 5000);
    require(rows.size() == files, "index and surviving files disagree after crash");
    for (const auto& row : rows) {
        const auto note = brain.read(row.id);
        require(note.has_value(), "surviving crash note cannot be parsed");
        require(note->body.find("crash-payload-") != std::string::npos &&
                    note->body.size() >= payload.size(),
                "surviving crash note was truncated");
    }
    SqliteDb db(home / "daedalus.db");
    require(db.one("PRAGMA integrity_check") ==
                std::optional<std::string>("ok"),
            "SQLite integrity check failed after killed WAL writer");
}

}  // anonymous namespace

int main()
{
    test_lock_modes_and_timeout();
    test_process_captures();
    test_concurrent_appends_and_readers();
    test_mixed_mutations_and_reindex();
    test_wal_and_index_transactions();
    test_reindex_rolls_back_as_one_transaction();
    test_sigkill_reopen_recovery();
    std::cout << "All SecondBrain concurrency and crash tests passed\n";
    return 0;
}
