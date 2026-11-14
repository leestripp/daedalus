#include "monkeybyte/backend/FileService.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

namespace monkeybyte {

namespace fs = std::filesystem;

FileService::FileService(fs::path workspace) {
    set_workspace(std::move(workspace));
}

void FileService::set_workspace(fs::path workspace) {
    std::error_code ec;
    workspace_ = fs::weakly_canonical(workspace, ec);
    if (ec) workspace_ = std::move(workspace);
}

std::expected<fs::path, std::string> FileService::resolve(const std::string& rel_or_abs) const {
    if (workspace_.empty())
        return std::unexpected("no workspace set");
    fs::path p(rel_or_abs);
    if (p.is_relative()) p = workspace_ / p;
    std::error_code ec;
    p = fs::weakly_canonical(p, ec);
    if (ec) return std::unexpected("invalid path: " + ec.message());
    const auto ws = workspace_.generic_string();
    const auto ps = p.generic_string();
    if (ps != ws && !ps.starts_with(ws + "/"))
        return std::unexpected("path escapes workspace: " + ps);
    return p;
}

void FileService::sort_entries(std::vector<Entry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;
        auto lower = [](std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return s;
        };
        return lower(a.name) < lower(b.name);
    });
}

std::expected<std::vector<FileService::Entry>, std::string>
FileService::list(const std::string& path) const {
    auto resolved = path.empty() ? std::expected<fs::path, std::string>(workspace_)
                                 : resolve(path);
    if (!resolved) return std::unexpected(resolved.error());

    std::error_code ec;
    if (!fs::is_directory(*resolved, ec))
        return std::unexpected("not a directory: " + resolved->string());

    std::vector<Entry> out;
    for (const auto& de : fs::directory_iterator(*resolved, ec)) {
        Entry e;
        e.name = de.path().filename().string();
        if (!show_hidden_ && !e.name.empty() && e.name.front() == '.')
            continue;
        e.path = de.path().string();
        e.is_dir = de.is_directory(ec);
        if (!e.is_dir) e.size = de.file_size(ec);
        out.push_back(std::move(e));
    }
    if (ec) return std::unexpected("cannot list: " + ec.message());
    sort_entries(out);
    return out;
}

std::expected<std::string, std::string> FileService::read(const std::string& path) const {
    auto resolved = resolve(path);
    if (!resolved) return std::unexpected(resolved.error());

    std::error_code ec;
    const auto size = fs::file_size(*resolved, ec);
    if (ec) return std::unexpected("cannot stat: " + ec.message());
    if (size > kMaxReadSize) return std::unexpected("file too large (>5 MB)");

    std::ifstream in(*resolved, std::ios::binary);
    if (!in) return std::unexpected("cannot open: " + resolved->string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::expected<void, std::string> FileService::write(const std::string& path,
                                                    const std::string& content) const {
    auto resolved = resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::ofstream out(*resolved, std::ios::binary | std::ios::trunc);
    if (!out) return std::unexpected("cannot write: " + resolved->string());
    out << content;
    if (!out) return std::unexpected("write failed: " + resolved->string());
    return {};
}

std::expected<void, std::string> FileService::mkdir(const std::string& path) const {
    auto resolved = resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    fs::create_directories(*resolved, ec);
    if (ec) return std::unexpected("mkdir failed: " + ec.message());
    return {};
}

std::expected<void, std::string> FileService::remove(const std::string& path) const {
    auto resolved = resolve(path);
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code ec;
    fs::remove_all(*resolved, ec);
    if (ec) return std::unexpected("delete failed: " + ec.message());
    return {};
}

std::expected<void, std::string> FileService::rename(const std::string& from,
                                                     const std::string& to) const {
    // Both endpoints must stay inside the workspace (§10 path sandboxing).
    auto src = resolve(from);
    if (!src) return std::unexpected(src.error());
    auto dst = resolve(to);
    if (!dst) return std::unexpected(dst.error());
    std::error_code ec;
    fs::rename(*src, *dst, ec);
    if (ec) return std::unexpected("rename failed: " + ec.message());
    return {};
}

std::expected<std::vector<FileService::Entry>, std::string>
FileService::browse_dirs(const fs::path& dir, bool show_hidden) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return std::unexpected("not a directory: " + dir.string());

    std::vector<Entry> out;
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_directory(ec)) continue;
        Entry e;
        e.name = de.path().filename().string();
        if (!show_hidden && !e.name.empty() && e.name.front() == '.') continue;
        e.path = de.path().string();
        e.is_dir = true;
        out.push_back(std::move(e));
    }
    if (ec) return std::unexpected("cannot list: " + ec.message());
    sort_entries(out);
    return out;
}

} // namespace monkeybyte
