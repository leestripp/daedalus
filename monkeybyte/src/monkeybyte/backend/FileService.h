#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <vector>

namespace monkeybyte {

// Workspace-rooted file service with path-traversal sandboxing (design §10).
class FileService {
public:
    struct Entry {
        std::string name;      // basename
        std::string path;      // absolute
        bool is_dir = false;
        std::uintmax_t size = 0;
    };

    static constexpr std::uintmax_t kMaxReadSize = 5 * 1024 * 1024; // 5 MB

    FileService() = default;
    explicit FileService(std::filesystem::path workspace);

    void set_workspace(std::filesystem::path workspace);
    const std::filesystem::path& workspace() const { return workspace_; }

    // When false (default), list() skips dotfile entries (.git, .env, …).
    void set_show_hidden(bool show) { show_hidden_ = show; }
    bool show_hidden() const { return show_hidden_; }

    // Resolve `rel_or_abs` against the workspace and verify it stays inside.
    std::expected<std::filesystem::path, std::string> resolve(const std::string& rel_or_abs) const;

    // List one directory. `path` may be "" (workspace root), relative, or absolute.
    std::expected<std::vector<Entry>, std::string> list(const std::string& path) const;
    std::expected<std::string, std::string> read(const std::string& path) const;
    std::expected<void, std::string> write(const std::string& path, const std::string& content) const;
    std::expected<void, std::string> mkdir(const std::string& path) const;
    std::expected<void, std::string> remove(const std::string& path) const;
    std::expected<void, std::string> rename(const std::string& from, const std::string& to) const;

    // List child *directories only* of an arbitrary absolute path (folder picker, §5.1).
    static std::expected<std::vector<Entry>, std::string> browse_dirs(
        const std::filesystem::path& dir, bool show_hidden);

    // Sort helper: directories first, then case-insensitive by name (§4.2).
    static void sort_entries(std::vector<Entry>& entries);

private:
    std::filesystem::path workspace_;
    bool show_hidden_ = false;
};

} // namespace monkeybyte
