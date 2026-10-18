// SPDX-License-Identifier: BSL-1.0
// src/core/SecondBrain.cpp — see SecondBrain.h for the design overview.
//
// §SECOND-BRAIN. Implementation notes:
//   - UTF-8 discipline (code-craft-cpp): std::string is BYTES. Every scan
//     below looks only for ASCII marker bytes ('-', '[', ']', ':', …), so
//     no multi-byte sequence can ever be split. slugify() keeps ASCII
//     alnum only and drops everything else whole.
//   - Frontmatter is parsed LENIENTLY: unknown keys ignored, missing keys
//     defaulted, a file with no frontmatter at all still reads as a note
//     (id = filename stem). We never refuse a human-edited file.
//   - All timestamps are UTC via gmtime_r (§MSG-HEADERS-UTC discipline) —
//     no localtime, no TZ state.

#include "SecondBrain.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "daedalus/paths.h"

namespace fs = std::filesystem;

namespace daedalus {

namespace {

// ---------------------------------------------------------------------------
// Small string helpers (file-local; operate on ASCII markers only)
// ---------------------------------------------------------------------------

std::string trim(std::string_view v)
{
    const auto first = v.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = v.find_last_not_of(" \t\r\n");
    return std::string(v.substr(first, last - first + 1));
}

std::string to_lower(std::string_view v)
{
    std::string out(v);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

/// Split "a, b, c" into trimmed non-empty parts.
std::vector<std::string> split_csv(std::string_view v)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= v.size()) {
        const auto comma = v.find(',', pos);
        const auto part = trim(v.substr(
            pos, comma == std::string_view::npos ? comma : comma - pos));
        if (!part.empty()) out.push_back(part);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return out;
}

/// Parse a YAML-ish inline list: "[a, b, 'c']" or a bare "a, b, c".
std::vector<std::string> parse_tag_list(std::string_view v)
{
    // trim() returns an owning string — keep it alive in a named local
    // before re-viewing (assigning the temporary to the view would dangle).
    const std::string trimmed = trim(v);
    v = trimmed;
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']')
        v = v.substr(1, v.size() - 2);
    std::vector<std::string> out;
    for (auto& t : split_csv(v)) {
        // Strip one layer of matching quotes around each item.
        if (t.size() >= 2 &&
            ((t.front() == '"' && t.back() == '"') ||
             (t.front() == '\'' && t.back() == '\'')))
            t = t.substr(1, t.size() - 2);
        if (!t.empty()) out.push_back(std::move(t));
    }
    return out;
}

std::string join_csv(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += v[i];
    }
    return out;
}

/// YAML double-quoted scalar: escape backslash and quote only.
std::string yaml_quote(std::string_view v)
{
    std::string out = "\"";
    for (const char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

/// Unquote a double-quoted YAML scalar (best-effort: \" and \\ escapes).
std::string yaml_unquote(std::string_view v)
{
    const std::string trimmed = trim(v);  // owning — see parse_tag_list
    v = trimmed;
    if (v.size() < 2 || v.front() != '"' || v.back() != '"')
        return std::string(v);
    std::string out;
    out.reserve(v.size() - 2);
    for (size_t i = 1; i + 1 < v.size(); ++i) {
        if (v[i] == '\\' && i + 2 < v.size() &&
            (v[i + 1] == '"' || v[i + 1] == '\\'))
            ++i;
        out += v[i];
    }
    return out;
}

std::string read_file_text(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + p.string());
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void write_file_text(const fs::path& p, std::string_view text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write " + p.string());
    out << text;
    out.flush();
    if (!out) throw std::runtime_error("write failed: " + p.string());
}

/// file_size + mtime as a seconds count. The epoch is the file_clock's
/// own — we only ever compare values produced by this same function, so
/// the unspecified epoch does not matter.
std::pair<int64_t, int64_t> file_stat(const fs::path& p)
{
    const auto size = static_cast<int64_t>(fs::file_size(p));
    const auto mtime = std::chrono::duration_cast<std::chrono::seconds>(
        fs::last_write_time(p).time_since_epoch()).count();
    return {mtime, size};
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / singleton
// ---------------------------------------------------------------------------

SecondBrain::SecondBrain(fs::path brain_root, SettingsStore store)
    : root_(std::move(brain_root)), store_(std::move(store))
{
    std::error_code ec;
    for (const auto& f : folders())
        fs::create_directories(root_ / f, ec);
    if (ec)
        throw std::runtime_error("SecondBrain: cannot create folders under "
                                 + root_.string() + ": " + ec.message());
}

SecondBrain& SecondBrain::global()
{
    // Magic static — thread-safe init. The brain lives beside the global
    // SettingsStore DB so every agent/project on this machine shares it.
    static SecondBrain instance(
        Paths::global().user_data_root() / "brain",
        SettingsStore::instance());
    return instance;
}

const std::vector<std::string>& SecondBrain::folders()
{
    static const std::vector<std::string> kFolders = {
        "inbox", "projects", "areas", "resources", "archives"
    };
    return kFolders;
}

bool SecondBrain::is_valid_folder(std::string_view folder)
{
    const auto& f = folders();
    return std::find(f.begin(), f.end(), folder) != f.end();
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

fs::path SecondBrain::folder_path(std::string_view folder) const
{
    return root_ / std::string(folder);
}

fs::path SecondBrain::note_path(std::string_view folder,
                                std::string_view id) const
{
    return root_ / std::string(folder) / (std::string(id) + ".md");
}

fs::path SecondBrain::find_note_file(std::string_view id) const
{
    // Path-traversal guard: ids are slugs — lowercase alnum and '-' only.
    // Anything else (notably '/', '.') never touches the filesystem.
    const bool safe = !id.empty() && std::all_of(id.begin(), id.end(),
        [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-';
        });
    if (!safe) return {};

    for (const auto& f : folders()) {
        const auto p = note_path(f, id);
        std::error_code ec;
        if (fs::exists(p, ec)) return p;
    }
    return {};
}

// ---------------------------------------------------------------------------
// Capture / read / update
// ---------------------------------------------------------------------------

BrainNote SecondBrain::capture(std::string_view folder,
                               std::string_view title,
                               const std::vector<std::string>& tags,
                               std::string_view body)
{
    std::lock_guard lk(mu_);

    if (!is_valid_folder(folder))
        throw std::invalid_argument(
            "SecondBrain::capture: invalid folder '" + std::string(folder) +
            "' (expected one of: " + join_csv(folders()) + ")");

    BrainNote note;
    note.folder = std::string(folder);
    note.title  = std::string(trim(title));
    note.tags   = tags;
    note.body   = std::string(body);
    // Trim trailing whitespace on the body but keep internal newlines.
    while (!note.body.empty() &&
           (note.body.back() == '\n' || note.body.back() == '\r' ||
            note.body.back() == ' ' || note.body.back() == '\t'))
        note.body.pop_back();

    // Title fallback: first markdown heading in the body, else "untitled".
    if (note.title.empty()) {
        std::istringstream lines(note.body);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("# ", 0) == 0) {
                note.title = trim(std::string_view(line).substr(2));
                break;
            }
        }
        if (note.title.empty()) note.title = "untitled";
    }

    // Unique id: clean slug first, then -2, -3, … on collision.
    const std::string base = slugify(note.title);
    note.id = base;
    for (int n = 2; !find_note_file(note.id).empty(); ++n) {
        if (n > 1000)
            throw std::runtime_error(
                "SecondBrain::capture: cannot find free id for slug " + base);
        note.id = base + "-" + std::to_string(n);
    }

    note.created = now_iso8601();
    note.updated = note.created;
    note.links   = extract_links(note.body);

    write_and_index(note);
    return note;
}

std::optional<BrainNote> SecondBrain::read(std::string_view id) const
{
    std::lock_guard lk(mu_);
    const auto path = find_note_file(id);
    if (path.empty()) return std::nullopt;
    return parse_file(path, path.parent_path().filename().string());
}

bool SecondBrain::update(std::string_view id,
                         const std::optional<std::string>& title,
                         const std::optional<std::vector<std::string>>& tags,
                         const std::optional<std::string>& body)
{
    std::lock_guard lk(mu_);
    auto note = read(id);
    if (!note.has_value()) return false;

    if (title.has_value()) note->title = *title;
    if (tags.has_value())  note->tags  = *tags;
    if (body.has_value())  note->body  = *body;
    note->updated = now_iso8601();
    note->links   = extract_links(note->body);

    write_and_index(*note);
    return true;
}

bool SecondBrain::append(std::string_view id, std::string_view markdown)
{
    std::lock_guard lk(mu_);
    auto note = read(id);
    if (!note.has_value()) return false;

    if (!note->body.empty()) note->body += "\n\n";
    note->body += std::string(markdown);
    note->updated = now_iso8601();
    note->links   = extract_links(note->body);

    write_and_index(*note);
    return true;
}

bool SecondBrain::remove(std::string_view id)
{
    std::lock_guard lk(mu_);
    const auto path = find_note_file(id);
    if (path.empty()) return false;

    std::error_code ec;
    fs::remove(path, ec);
    if (ec)
        throw std::runtime_error("SecondBrain::remove: cannot delete "
                                 + path.string() + ": " + ec.message());
    store_.delete_brain_note(id);
    return true;
}

// ---------------------------------------------------------------------------
// Organise
// ---------------------------------------------------------------------------

bool SecondBrain::move(std::string_view id, std::string_view new_folder)
{
    std::lock_guard lk(mu_);
    if (!is_valid_folder(new_folder))
        throw std::invalid_argument(
            "SecondBrain::move: invalid folder '" + std::string(new_folder) +
            "'");

    auto note = read(id);
    if (!note.has_value()) return false;
    if (note->folder == new_folder) return true;  // already there — no-op

    const auto old_path = note_path(note->folder, note->id);
    note->folder = std::string(new_folder);
    note->updated = now_iso8601();

    // Write the new file first, then remove the old one — a crash between
    // the two leaves a duplicate (reindex reconciles), never a lost note.
    write_and_index(*note);
    std::error_code ec;
    fs::remove(old_path, ec);
    return true;
}

std::vector<std::string> SecondBrain::backlinks(std::string_view id) const
{
    std::lock_guard lk(mu_);
    std::vector<std::string> out;
    const std::string target(id);

    for (const auto& folder : folders()) {
        const auto dir = folder_path(folder);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".md") continue;
            const auto stem = entry.path().stem().string();
            if (stem == target) continue;  // a note is not its own backlink
            auto note = parse_file(entry.path(), folder);
            if (!note.has_value()) continue;
            if (std::find(note->links.begin(), note->links.end(), target)
                    != note->links.end())
                out.push_back(stem);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// Find
// ---------------------------------------------------------------------------

std::vector<BrainNoteMeta> SecondBrain::list(std::string_view folder,
                                             std::string_view tag,
                                             int limit) const
{
    std::lock_guard lk(mu_);
    if (!folder.empty() && !is_valid_folder(folder))
        throw std::invalid_argument(
            "SecondBrain::list: invalid folder '" + std::string(folder) +
            "'");

    // Tag filtering happens in C++ (tags are a CSV column); when a tag is
    // given we over-fetch so the post-filter limit stays meaningful.
    const int fetch = tag.empty() ? limit : limit * 4 + 20;
    auto rows = store_.list_brain_notes(folder, fetch);

    if (!tag.empty()) {
        const std::string want(tag);
        std::erase_if(rows, [&](const BrainNoteMeta& r) {
            const auto tags = split_csv(r.tags);
            return std::find(tags.begin(), tags.end(), want) == tags.end();
        });
    }
    if (static_cast<int>(rows.size()) > limit) rows.resize(size_t(limit));
    return rows;
}

std::vector<BrainSearchHit> SecondBrain::search(std::string_view query,
                                                int limit) const
{
    std::lock_guard lk(mu_);
    const auto fts = sanitise_fts_query(query);
    if (fts.empty()) return {};
    return store_.search_brain_notes(fts, limit);
}

// ---------------------------------------------------------------------------
// Index maintenance
// ---------------------------------------------------------------------------

SecondBrain::ReindexResult SecondBrain::reindex()
{
    std::lock_guard lk(mu_);
    ReindexResult result;

    // Current index state, keyed by id.
    std::unordered_map<std::string, BrainNoteMeta> indexed;
    for (const auto& row : store_.all_brain_notes())
        indexed.emplace(row.id, row);

    std::unordered_set<std::string> seen_on_disk;

    for (const auto& folder : folders()) {
        const auto dir = folder_path(folder);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".md") continue;

            const auto stem = entry.path().stem().string();
            seen_on_disk.insert(stem);

            const auto [mtime, size] = file_stat(entry.path());
            const auto it = indexed.find(stem);
            const bool fresh = it != indexed.end() &&
                it->second.mtime == mtime && it->second.size == size &&
                it->second.folder == folder;
            if (fresh) continue;

            auto note = parse_file(entry.path(), folder);
            if (!note.has_value()) continue;
            BrainNoteMeta row;
            row.id      = note->id;
            row.folder  = note->folder;
            row.title   = note->title;
            row.tags    = join_csv(note->tags);
            row.created = note->created;
            row.updated = note->updated;
            row.mtime   = mtime;
            row.size    = size;
            store_.upsert_brain_note(row, note->body);
            ++result.indexed;
        }
    }

    // Drop index rows whose files have vanished.
    for (const auto& [id, row] : indexed) {
        if (seen_on_disk.find(id) == seen_on_disk.end()) {
            store_.delete_brain_note(id);
            ++result.removed;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void SecondBrain::write_and_index(const BrainNote& note)
{
    // Caller holds mu_.
    const auto path = note_path(note.folder, note.id);
    write_file_text(path, render_file(note));

    const auto [mtime, size] = file_stat(path);
    BrainNoteMeta row;
    row.id      = note.id;
    row.folder  = note.folder;
    row.title   = note.title;
    row.tags    = join_csv(note.tags);
    row.created = note.created;
    row.updated = note.updated;
    row.mtime   = mtime;
    row.size    = size;
    store_.upsert_brain_note(row, note.body);
}

std::optional<BrainNote> SecondBrain::parse_file(const fs::path& path,
                                                 std::string_view folder)
{
    std::string text;
    try {
        text = read_file_text(path);
    } catch (const std::exception&) {
        return std::nullopt;  // unreadable file — skip, never propagate
    }

    BrainNote note;
    note.id     = path.stem().string();
    note.folder = std::string(folder);

    std::string body = text;

    // Frontmatter: file must START with a "---" line; the block ends at
    // the next "---" line. Anything else means "no frontmatter".
    if (text.rfind("---\n", 0) == 0 || text.rfind("---\r\n", 0) == 0) {
        std::istringstream lines(text);
        std::string line;
        std::getline(lines, line);  // consume opening "---"
        bool closed = false;
        while (std::getline(lines, line)) {
            const auto t = trim(line);
            if (t == "---") { closed = true; break; }
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;  // lenient: skip
            const auto key   = to_lower(trim(
                std::string_view(line).substr(0, colon)));
            const auto value = trim(std::string_view(line).substr(colon + 1));

            // NB: the "id" key is deliberately NOT honoured on read.
            // Design: filename = note id. A hand-edited frontmatter id
            // could otherwise (a) inject "../" into note_path() via
            // update()/move() — an arbitrary-write traversal — and
            // (b) desync reindex (row id vs filename stem churn).
            if (key == "title")      note.title   = yaml_unquote(value);
            else if (key == "tags")  note.tags    = parse_tag_list(value);
            else if (key == "created") note.created = yaml_unquote(value);
            else if (key == "updated") note.updated = yaml_unquote(value);
            // unknown keys: ignored by design
        }
        if (closed) {
            // Body = everything after the closing marker line.
            std::ostringstream b;
            bool first = true;
            while (std::getline(lines, line)) {
                if (!first) b << '\n';
                b << line;
                first = false;
            }
            body = b.str();
            // Strip leading blank lines left by the "---\n\nbody" layout.
            while (body.rfind("\n", 0) == 0 || body.rfind("\r", 0) == 0)
                body.erase(body.begin());
        }
    }

    // Defaults for missing frontmatter keys.
    if (note.title.empty()) {
        std::istringstream lines(body);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.rfind("# ", 0) == 0) {
                note.title = trim(std::string_view(line).substr(2));
                break;
            }
        }
        if (note.title.empty()) note.title = note.id;
    }

    note.body  = std::move(body);
    note.links = extract_links(note.body);
    return note;
}

std::vector<std::string> SecondBrain::extract_links(std::string_view body)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = body.find("[[", pos)) != std::string_view::npos) {
        const auto end = body.find("]]", pos + 2);
        if (end == std::string_view::npos) break;
        auto target = trim(body.substr(pos + 2, end - pos - 2));
        // Obsidian alias form: [[id|display text]] — keep the id part.
        if (const auto bar = target.find('|'); bar != std::string::npos)
            target = trim(std::string_view(target).substr(0, bar));
        if (!target.empty() &&
            std::find(out.begin(), out.end(), target) == out.end())
            out.push_back(std::move(target));
        pos = end + 2;
    }
    return out;
}

std::string SecondBrain::render_file(const BrainNote& note)
{
    std::ostringstream out;
    out << "---\n";
    out << "id: " << note.id << "\n";
    out << "title: " << yaml_quote(note.title) << "\n";
    out << "tags: [";
    for (size_t i = 0; i < note.tags.size(); ++i) {
        if (i) out << ", ";
        // Quote only when the tag contains something beyond slug chars.
        const auto& t = note.tags[i];
        const bool plain = std::all_of(t.begin(), t.end(),
            [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            });
        out << (plain ? t : yaml_quote(t));
    }
    out << "]\n";
    out << "created: " << note.created << "\n";
    out << "updated: " << note.updated << "\n";
    out << "---\n\n";
    out << note.body;
    out << '\n';
    return out.str();
}

std::string SecondBrain::slugify(std::string_view title)
{
    // ASCII alnum runs joined by '-'; everything else (including every
    // byte of a multi-byte UTF-8 sequence) is a separator — so a sequence
    // can never be split, it simply collapses into the join.
    std::string out;
    out.reserve(title.size());
    bool dash = false;
    for (const unsigned char c : title) {
        if (std::isalnum(c)) {
            out += char(std::tolower(c));
            dash = false;
        } else if (!dash && !out.empty()) {
            out += '-';
            dash = true;
        }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    constexpr size_t kMaxSlug = 48;  // keep filenames manageable
    if (out.size() > kMaxSlug) {
        out.resize(kMaxSlug);
        while (!out.empty() && out.back() == '-') out.pop_back();
    }
    return out.empty() ? "untitled" : out;
}

std::string SecondBrain::sanitise_fts_query(std::string_view query)
{
    // Term characters: ASCII alnum plus ANY byte >= 0x80. Every FTS5
    // query-syntax metacharacter ( " ( ) : * ^ + - whitespace ) is
    // ASCII, so UTF-8 bytes can never inject syntax — they flow into
    // terms whole (multi-byte sequences are never split: each byte
    // simply joins the current run), letting unicode61-tokenised
    // non-ASCII text (CJK, Cyrillic, accented Latin) actually match.
    // Terms become prefix tokens ("term*") joined with AND.
    std::vector<std::string> terms;
    std::string current;
    for (const unsigned char c : query) {
        if (std::isalnum(c) || c >= 0x80) {
            current += char(std::tolower(c));
        } else if (!current.empty()) {
            terms.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) terms.push_back(std::move(current));

    std::string out;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i) out += " AND ";
        out += terms[i];
        out += '*';
    }
    return out;
}

std::string SecondBrain::now_iso8601()
{
    // §MSG-HEADERS-UTC: UTC always, via gmtime_r (no TZ state touched).
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace daedalus
