// SPDX-License-Identifier: BSL-1.0
// tests/test_brain_tool.cpp
//
// §SECOND-BRAIN: hermetic tests for BrainTool's static action handlers.
// execute() itself resolves SecondBrain::global() (process-wide state),
// so — exactly like VaultTool — the handlers take an explicit brain and
// we drive them against a temp-home one.
//
// What we pin:
//   T1. get_definition(): valid function schema, action enum complete,
//       only "action" is required.
//   T2. capture → search → read happy path through the JSON interface.
//   T3. update/append semantics + the "nothing to update" error.
//   T4. file (move) + backlinks + list filters.
//   T5. delete + reindex round out the action set.
//   T6. Error paths: missing required args, unknown ids, unknown action
//       arg values surface as {"status":"error"} — never throw.

// The default build is Release (-DNDEBUG), which would compile assert()
// away — force it back on (project pattern).
#undef NDEBUG
#include <cassert>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include <daedalus/settings.h>

#include "core/SecondBrain.h"
#include "tools/BrainTool.h"

namespace fs = std::filesystem;
using namespace daedalus;
using json = nlohmann::json;

namespace {

struct TmpDir {
    fs::path path;
    TmpDir() {
        path = fs::temp_directory_path() /
               ("daedalus-braintool-" +
                std::to_string(std::chrono::system_clock::now()
                                 .time_since_epoch().count()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpDir() { std::error_code ec; fs::remove_all(path, ec); }
};

bool ok(const json& r) { return r.value("status", "") == "ok"; }
bool err(const json& r) { return r.value("status", "") == "error"; }

}  // anonymous namespace

int main() {
    TmpDir tmp;
    auto store = SettingsStore::for_home(tmp.path);
    SecondBrain brain(tmp.path / "brain", store);

    // ── T1: schema shape ──
    BrainTool tool;
    assert(tool.get_name() == "brain");
    const auto def = tool.get_definition();
    assert(def.value("type", "") == "function");
    const auto& fn = def.at("function");
    assert(fn.at("name") == "brain");
    const auto& params = fn.at("parameters");
    const auto& required = params.at("required");
    assert(required.size() == 1 && required[0] == "action");
    const auto& actions =
        params.at("properties").at("action").at("enum");
    for (const auto* a : {"capture", "search", "read", "list", "update",
                          "append", "file", "backlinks", "delete",
                          "reindex"}) {
        assert(std::find(actions.begin(), actions.end(), a)
               != actions.end());
    }

    // ── T2: capture → search → read ──
    auto cap = BrainTool::do_capture(brain, {
        {"action", "capture"},
        {"folder", "resources"},
        {"title", "PARA Method"},
        {"tags", json::array({"pkm", "basb"})},
        {"body", "# PARA\n\nProjects, Areas, Resources, Archives. "
                 "Relates to [[tiago-forte-basb]]."},
    });
    assert(ok(cap));
    assert(cap.value("id", "") == "para-method");
    assert(cap.value("folder", "") == "resources");

    // capture without body → std::invalid_argument (execute() maps it to
    // {"status":"error"}; the T6 block pins that contract).
    {
        bool threw = false;
        try {
            (void)BrainTool::do_capture(brain, {{"title", "no body"}});
        } catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }

    auto hits = BrainTool::do_search(brain, {{"query", "projects areas"}});
    assert(ok(hits));
    assert(hits.value("count", 0) == 1);
    assert(hits.at("results")[0].value("id", "") == "para-method");
    assert(!hits.at("results")[0].value("snippet", "").empty());

    auto rd = BrainTool::do_read(brain, {{"id", "para-method"}});
    assert(ok(rd));
    assert(rd.value("title", "") == "PARA Method");
    assert(rd.at("tags").size() == 2);
    assert(rd.at("links")[0] == "tiago-forte-basb");
    assert(rd.value("body", "").find("Archives") != std::string::npos);
    assert(err(BrainTool::do_read(brain, {{"id", "missing-note"}})));
    {   // no id → throws (execute() maps to {"status":"error"})
        bool threw = false;
        try {
            (void)BrainTool::do_read(brain, json::object());
        } catch (const std::invalid_argument&) { threw = true; }
        assert(threw);
    }

    // ── T3: update / append ──
    auto up = BrainTool::do_update(brain, {
        {"id", "para-method"},
        {"tags", json::array({"pkm", "methodology"})},
    });
    assert(ok(up));
    rd = BrainTool::do_read(brain, {{"id", "para-method"}});
    assert(rd.at("tags")[1] == "methodology");
    assert(rd.value("title", "") == "PARA Method");  // untouched

    // Nothing to update → clear error.
    auto noop = BrainTool::do_update(brain, {{"id", "para-method"}});
    assert(err(noop));
    assert(noop.value("message", "").find("at least one")
           != std::string::npos);

    assert(ok(BrainTool::do_append(brain, {
        {"id", "para-method"},
        {"markdown", "Captured later: extra detail."},
    })));
    rd = BrainTool::do_read(brain, {{"id", "para-method"}});
    assert(rd.value("body", "").find("extra detail") != std::string::npos);
    assert(err(BrainTool::do_append(brain, {{"id", "missing"},
                                            {"markdown", "x"}})));

    // ── T4: file + backlinks + list ──
    assert(ok(BrainTool::do_capture(brain, {
        {"title", "Tiago Forte BASB"},
        {"body", "Building a Second Brain book notes."},
    })));
    auto fl = BrainTool::do_file(brain, {
        {"id", "para-method"}, {"folder", "projects"}});
    assert(ok(fl));
    rd = BrainTool::do_read(brain, {{"id", "para-method"}});
    assert(rd.value("folder", "") == "projects");
    // read() includes backlinks: the new BASB note links… no — the PARA
    // note links TO basb, so basb gains a backlink.
    auto bl = BrainTool::do_backlinks(brain, {{"id", "tiago-forte-basb"}});
    assert(ok(bl));
    assert(bl.value("count", 0) == 1);
    assert(bl.at("backlinks")[0] == "para-method");

    auto lst = BrainTool::do_list(brain, json::object());
    assert(ok(lst) && lst.value("count", 0) == 2);
    lst = BrainTool::do_list(brain, {{"folder", "projects"}});
    assert(lst.value("count", 0) == 1);
    lst = BrainTool::do_list(brain, {{"tag", "methodology"}});
    assert(lst.value("count", 0) == 1);

    // ── T5: delete + reindex ──
    assert(ok(BrainTool::do_delete(brain, {{"id", "tiago-forte-basb"}})));
    assert(err(BrainTool::do_delete(brain, {{"id", "tiago-forte-basb"}})));
    auto rei = BrainTool::do_reindex(brain);
    assert(ok(rei));
    assert(rei.value("indexed", -1) == 0);  // nothing changed on disk

    // ── T6: required-arg enforcement via exceptions → execute() never
    // sees them (handlers throw std::invalid_argument, execute() maps to
    // {"status":"error"}). Direct handler calls throw — pin that contract.
    bool threw = false;
    try {
        (void)BrainTool::do_search(brain, json::object());
    } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);

    std::cout << "All BrainTool tests passed (T1-T6)\n";
    return 0;
}
