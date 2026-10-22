// SPDX-License-Identifier: BSL-1.0
// tests/test_history_snapshot.cpp
//
// §HISTORY-SNAPSHOT (2026-09-10, MonkeyByte upstream report REQ-2):
// Messages::history_snapshot() projects the live chain into
// host-renderable HistoryEntry records — system/user/assistant text
// only; tool calls and tool results elided; multipart (vision) content
// flattened to its text parts.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/Messages.h"

namespace fs = std::filesystem;

namespace {

fs::path make_test_project() {
    fs::path proj = fs::temp_directory_path() /
        ("daedalus_history_snapshot_test_" + std::to_string(getpid()));
    fs::create_directories(proj / ".daedalus" / "daedalus");
    return proj;
}

Messages make_messages(const fs::path& proj_root) {
    MessageCompressionConfig config;
    config.max_context_size = 200000;
    config.debug_output = false;
    config.auto_flush_enabled = false;
    return Messages(proj_root / ".daedalus" / "daedalus", config);
}

}  // namespace

TEST_SUITE("HISTORY-SNAPSHOT") {

TEST_CASE("empty chain yields empty snapshot") {
    const fs::path proj = make_test_project();
    {
        auto msgs = make_messages(proj);
        CHECK(msgs.history_snapshot().empty());
    }
    fs::remove_all(proj);
}

TEST_CASE("text roles round-trip; tool plumbing elided") {
    const fs::path proj = make_test_project();
    {
        auto msgs = make_messages(proj);
        msgs.add_system("you are helpful");
        msgs.add_user("hi");
        msgs.add_assistant("hello there");
        // pure tool-call assistant message — must be elided
        msgs.add_message({
            {"role", "assistant"},
            {"tool_calls", json::array({{
                {"id", "call_1"},
                {"type", "function"},
                {"function", {{"name", "execute_bash"},
                              {"arguments", R"({"command":"ls"})"}}},
            }})},
        });
        // tool result — must be elided
        msgs.add_tool_result("call_1", "file.txt");
        msgs.add_user("thanks");

        const auto snap = msgs.history_snapshot();
        REQUIRE(snap.size() == 4);
        CHECK(snap[0].role == "system");
        CHECK(snap[0].content == "you are helpful");
        CHECK(snap[1].role == "user");
        CHECK(snap[1].content == "hi");
        CHECK(snap[2].role == "assistant");
        CHECK(snap[2].content == "hello there");
        CHECK(snap[3].role == "user");
        CHECK(snap[3].content == "thanks");
    }
    fs::remove_all(proj);
}

TEST_CASE("multipart content flattens to text parts") {
    const fs::path proj = make_test_project();
    {
        auto msgs = make_messages(proj);
        msgs.add_message({
            {"role", "user"},
            {"content", json::array({
                {{"type", "text"}, {"text", "what is in this image?"}},
                {{"type", "image_url"},
                 {"image_url", {{"url", "data:image/png;base64,AAAA"}}}},
            })},
        });

        const auto snap = msgs.history_snapshot();
        REQUIRE(snap.size() == 1);
        CHECK(snap[0].role == "user");
        CHECK(snap[0].content == "what is in this image?");
    }
    fs::remove_all(proj);
}

TEST_CASE("malformed entries are skipped, not fatal") {
    const fs::path proj = make_test_project();
    {
        auto msgs = make_messages(proj);
        msgs.add_user("good");
        msgs.add_message(json::array({1, 2, 3}));           // non-object
        msgs.add_message({{"role", 42}});                   // non-string role
        msgs.add_message({{"role", "assistant"}});          // no content

        const auto snap = msgs.history_snapshot();
        REQUIRE(snap.size() == 1);
        CHECK(snap[0].content == "good");
    }
    fs::remove_all(proj);
}

TEST_CASE("assistant text+tool_calls keeps the text (final answers)") {
    const fs::path proj = make_test_project();
    {
        auto msgs = make_messages(proj);
        msgs.add_user("ping");
        // daedalus terminates a turn by attaching the `stop` tool call to
        // the assistant message that ALSO carries the final answer text —
        // the restored host UI must show that text, not elide it.
        msgs.add_message({
            {"role", "assistant"},
            {"content", "PONG"},
            {"tool_calls", json::array({{
                {"id", "call_stop"},
                {"type", "function"},
                {"function", {{"name", "stop"},
                              {"arguments", R"({"summary":"done"})"}}},
            }})},
        });
        msgs.add_tool_result("call_stop", R"({"status":"stop"})");

        const auto snap = msgs.history_snapshot();
        // user + assistant answer; tool plumbing elided.
        REQUIRE(snap.size() == 2);
        CHECK(snap[0].role == "user");
        CHECK(snap[1].role == "assistant");
        CHECK(snap[1].content == "PONG");
    }
    fs::remove_all(proj);
}

}  // TEST_SUITE
