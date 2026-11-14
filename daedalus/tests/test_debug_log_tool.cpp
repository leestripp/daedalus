// SPDX-License-Identifier: BSL-1.0
// tests/test_debug_log_tool.cpp
//
// §DEBUG-LOG: hermetic tests for DebugLogTool (shared agent/app debug log).

// The default build is Release (-DNDEBUG), which would compile assert()
// away — force it back on (project pattern).
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "core/FileIO.h"
#include "tools/DebugLogTool.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct ProjectRoot {
    fs::path root;
    fs::path work;
    ProjectRoot() {
        root = fs::temp_directory_path() /
               ("daedalus-debuglog-" +
                std::to_string(std::chrono::system_clock::now()
                                  .time_since_epoch().count()));
        work = root / "work";
        fs::create_directories(work);
        std::ofstream(root / ".git").put('\n');  // §PROJECT-MARKERS
    }
    ~ProjectRoot() { std::error_code ec; fs::remove_all(root, ec); }
};

bool ok(const json& r) { return r.value("status", "") == "ok"; }
bool err(const json& r) { return r.value("status", "") == "error"; }

}  // anonymous namespace

int main() {
    // ── T1: append → tail round trip, UTC timestamp + [agent] tag.
    {
        ProjectRoot project;
        const auto path = project.root / "custom-debug.log";
        FileIO file_io(project.work);
        DebugLogTool tool;

        assert(ok(tool.execute({{"action", "append"}, {"message", "first line"},
                                {"path", path.string()}}, file_io)));
        assert(ok(tool.execute({{"action", "append"}, {"message", "second line"},
                                {"path", path.string()}}, file_io)));

        const auto tail = tool.execute({{"action", "tail"}, {"path", path.string()}}, file_io);
        assert(ok(tail));
        assert(tail.at("total") == 2);
        const auto lines = tail.at("lines").get<std::vector<std::string>>();
        assert(lines.size() == 2);
        assert(lines[0].find("[agent] first line") != std::string::npos);
        assert(lines[1].find("[agent] second line") != std::string::npos);
        // ISO-8601 UTC prefix "YYYY-MM-DDT…".
        assert(lines[0].size() > 22);
        assert(lines[0][4] == '-' && lines[0][10] == 'T' && lines[0].back() != '\r');
    }

    // ── T2: default path lands under <project>/.daedalus.
    {
        ProjectRoot project;
        FileIO file_io(project.work);
        DebugLogTool tool;

        const auto result = tool.execute({{"message", "default path check"}}, file_io);
        assert(ok(result));
        assert(result.at("path") == (project.work / ".daedalus" / "app-debug.log").string());
        assert(fs::exists(project.work / ".daedalus" / "app-debug.log"));
    }

    // ── T3: tail honours the lines limit; missing file is empty, not error.
    {
        ProjectRoot project;
        const auto path = project.root / "limited.log";
        FileIO file_io(project.work);
        DebugLogTool tool;

        const auto missing = tool.execute({{"action", "tail"}, {"path", path.string()}}, file_io);
        assert(ok(missing));
        assert(missing.at("total") == 0);
        assert(missing.at("lines").empty());

        for (int index = 0; index < 10; ++index)
            tool.execute({{"message", "line " + std::to_string(index)},
                          {"path", path.string()}}, file_io);
        const auto tail = tool.execute({{"action", "tail"}, {"lines", 3},
                                        {"path", path.string()}}, file_io);
        assert(tail.at("total") == 10);
        const auto lines = tail.at("lines").get<std::vector<std::string>>();
        assert(lines.size() == 3);
        assert(lines[2].find("line 9") != std::string::npos);
    }

    // ── T4: error paths never throw; definition is a valid function schema.
    {
        ProjectRoot project;
        FileIO file_io(project.work);
        DebugLogTool tool;

        assert(err(tool.execute({{"action", "append"}}, file_io)));        // no message
        assert(err(tool.execute({{"action", "nonsense"}}, file_io)));      // unknown action

        const auto definition = tool.get_definition();
        assert(definition.at("function").at("name") == "debug_log");
        assert(definition.at("function").at("parameters").at("properties").contains("action"));
        assert(definition.at("function").at("parameters").at("properties").contains("lines"));
    }

    std::cout << "All DebugLogTool tests passed (T1-T4)\n";
    return 0;
}
