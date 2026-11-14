#include "monkeybyte/backend/SearchService.h"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <system_error>

namespace monkeybyte {

namespace {

constexpr std::size_t kSniffSize = 8192;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

// grep/ripgrep-style content sniff (§4.3): NUL byte or >30% non-printable
// bytes in the first chunk marks the file as binary. Bytes >= 128 count as
// printable so UTF-8 text stays searchable.
bool looks_binary(const char* data, std::size_t n) {
    if (n == 0) return false;
    if (std::memchr(data, '\0', n)) return true;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        const bool printable = c >= 128 || (c >= 32 && c <= 126) || c == '\t' ||
                               c == '\n' || c == '\r' || c == '\f' || c == '\b';
        if (!printable) ++bad;
    }
    return bad * 10 > n * 3;
}

// Glob match against the file name or the workspace-relative path
// (fnmatch with no flags: '*' may cross '/'). §4.3 Settings → Search.
bool is_excluded(const std::string& name, const std::string& rel_path,
                 const std::vector<std::string>& excludes) {
    for (const auto& pat : excludes)
        if (fnmatch(pat.c_str(), name.c_str(), 0) == 0 ||
            fnmatch(pat.c_str(), rel_path.c_str(), 0) == 0)
            return true;
    return false;
}

} // namespace

std::vector<SearchService::Result>
SearchService::search(const std::filesystem::path& root, const std::string& query,
                      const std::vector<std::string>& excludes) {
    std::vector<Result> out;
    if (query.empty()) return out;
    const std::string needle = lower(query);

    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    const std::filesystem::recursive_directory_iterator end;

    for (; it != end && out.size() < kMaxResults; it.increment(ec)) {
        const auto& p = it->path();
        const std::string name = p.filename().string();
        std::string rel; // workspace-relative, only computed when needed
        if (!excludes.empty())
            rel = std::filesystem::relative(p, root, ec).generic_string();
        if (it->is_directory(ec)) {
            if (!name.empty() && name.front() == '.')
                it.disable_recursion_pending();
            else if (!excludes.empty() && is_excluded(name, rel, excludes))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        if (!excludes.empty() && is_excluded(name, rel, excludes)) continue;
        if (it->file_size(ec) > 5 * 1024 * 1024) continue;

        std::ifstream in(p, std::ios::binary);
        if (!in) continue;

        // Skip binary files (a.out, images, …): sniff the first chunk.
        char sniff[kSniffSize];
        in.read(sniff, kSniffSize);
        if (looks_binary(sniff, static_cast<std::size_t>(in.gcount()))) continue;
        in.clear();
        in.seekg(0);

        std::string line;
        int line_no = 0;
        while (std::getline(in, line)) {
            ++line_no;
            if (lower(line).find(needle) == std::string::npos) continue;
            if (line.size() > kMaxPreview) {
                line.resize(kMaxPreview);
                line += "…";
            }
            out.push_back(Result{p.string(), line_no, line});
            break; // one row per file, like the web app's preview list
        }
    }
    return out;
}

} // namespace monkeybyte
