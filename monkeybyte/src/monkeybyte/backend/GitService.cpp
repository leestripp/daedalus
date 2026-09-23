#include "monkeybyte/backend/GitService.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <sys/wait.h>

namespace monkeybyte {

namespace {

std::string run_git(const std::filesystem::path& dir, const std::string& args, int& exit_code) {
    const std::string cmd = "git -C \"" + dir.string() + "\" " + args + " 2>/dev/null";
    std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(cmd.c_str(), "r"), &pclose);
    if (!pipe) throw std::runtime_error("GitService: popen failed");
    std::array<char, 4096> buf{};
    std::string out;
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe.get()))
        out += buf.data();
    const int rc = pclose(pipe.release());
    exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return out;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

} // namespace

GitService::Status GitService::status(const std::filesystem::path& dir) {
    Status st;
    int rc = 0;
    run_git(dir, "rev-parse --is-inside-work-tree", rc);
    if (rc != 0) return st; // not a repo

    st.is_repo = true;
    st.branch = trim(run_git(dir, "branch --show-current", rc));
    if (st.branch.empty())
        st.branch = trim(run_git(dir, "rev-parse --short HEAD", rc));

    const std::string porcelain = run_git(dir, "status --porcelain=v1", rc);
    std::istringstream lines(porcelain);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.size() < 4) continue;
        Change c;
        // porcelain: XY <path>; untracked shows as "??"
        c.status = line[0] != ' ' ? line[0] : line[1];
        c.path = trim(line.substr(3));
        if (const auto arrow = c.path.find(" -> "); arrow != std::string::npos)
            c.path = c.path.substr(arrow + 4);
        st.changes.push_back(std::move(c));
    }
    return st;
}

} // namespace monkeybyte
