#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace monkeybyte {

// Read-only git status via the `git` CLI (design §4.4 — no staging/commit UI).
class GitService {
public:
    struct Change {
        char status = '?'; // M / A / D / ? / R ...
        std::string path;
    };

    struct Status {
        bool is_repo = false;
        std::string branch;
        std::vector<Change> changes;
    };

    // Runs `git -C <dir> ...`. Errors (missing git, not a repo) are folded
    // into the returned Status (is_repo=false) or thrown as runtime_error
    // only for unexpected subprocess failures.
    static Status status(const std::filesystem::path& dir);
};

} // namespace monkeybyte
