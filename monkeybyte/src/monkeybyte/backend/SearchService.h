#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace monkeybyte {

// Recursive case-insensitive content search over the workspace (design §4.3).
class SearchService {
public:
    struct Result {
        std::string path;    // absolute file path
        int line_no = 0;
        std::string preview; // matching line, ellipsized to 200 chars
    };

    static constexpr std::size_t kMaxPreview = 200;
    static constexpr std::size_t kMaxResults = 200;

    // Searches text files under `root` for `query` (case-insensitive substring).
    // Skips hidden dirs, .git, files larger than 5 MB, binary files
    // (content sniffed, first 8 KB), and anything matching `excludes`
    // (glob patterns vs file name and workspace-relative path, §4.3).
    static std::vector<Result> search(const std::filesystem::path& root,
                                      const std::string& query,
                                      const std::vector<std::string>& excludes = {});
};

} // namespace monkeybyte
