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
        std::string upstream; // empty when HEAD has no configured upstream
        bool divergence_valid = false;
        unsigned int ahead = 0;  // local commits not yet on the upstream
        unsigned int behind = 0; // upstream commits not yet in the local branch
        std::vector<Change> changes;
    };

    // Runs `git -C <dir> ...`. Errors (missing git, not a repo) are folded
    // into the returned Status (is_repo=false) or thrown as runtime_error
    // only for unexpected subprocess failures.
    static Status status(const std::filesystem::path& dir);

    // Refresh the configured upstream's remote-tracking refs without changing
    // the working tree. The fetch is non-interactive and bounded so a missing
    // credential or unavailable network cannot wedge the status-bar worker.
    static bool fetch_upstream(const std::filesystem::path& dir);
};

} // namespace monkeybyte
