/**
 * @file test_message_headers.cpp
 * @brief §MSG-HEADERS (2026-10-04): unit tests for the message origin
 *        envelope — `_meta.source` + `_meta.received_at` stamped on
 *        every message at creation, and `wire_render()` which expands
 *        `_meta` into a visible [from: … · received …] content header
 *        on the provider wire copy while stripping the `_meta` key.
 *
 * Motivation: without origin/time the model cannot tell an old,
 * already-addressed message (e.g. a context-compression summary, a
 * re-fired trigger, a crash-restored prompt) from a fresh one.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <filesystem>
#include <unistd.h>

#include "core/Messages.h"

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

fs::path make_scratch() {
    fs::path p = fs::temp_directory_path() /
        ("daedalus_msgheaders_test_" + std::to_string(getpid()));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

Messages make_messages(const fs::path& dir) {
    MessageCompressionConfig cfg;
    cfg.auto_flush_enabled = false;  // tests control save() timing
    return Messages(dir, cfg);
}

} // namespace

TEST_SUITE("§MSG-HEADERS message origin envelope") {

    TEST_CASE("stamp writes _meta.source and a non-empty received_at") {
        json msg = {{"role", "user"}, {"content", "hello"}};
        Messages::stamp(msg, "trigger:nightly-backup");

        REQUIRE(msg.contains("_meta"));
        CHECK(msg["_meta"].value("source", "") == "trigger:nightly-backup");
        CHECK(!msg["_meta"].value("received_at", "").empty());
    }

    TEST_CASE("stamp with empty source leaves a message untouched") {
        json msg = {{"role", "user"}, {"content", "hello"}};
        Messages::stamp(msg, "");  // replay path: preserve loaded _meta
        CHECK(!msg.contains("_meta"));
    }

    TEST_CASE("now_string is UTC and matches YYYY-MM-DD HH:MM:SS shape") {
        const std::string now = Messages::now_string();
        REQUIRE(now.size() == 23);  // "%Y-%m-%d %H:%M:%S" + " UTC"
        CHECK(now[4] == '-');
        CHECK(now[7] == '-');
        CHECK(now[10] == ' ');
        CHECK(now[13] == ':');
        CHECK(now[16] == ':');
        CHECK(now.substr(19) == " UTC");  // §MSG-HEADERS-UTC: always UTC
    }

    TEST_CASE("add_* stamp source and received_at by default") {
        const fs::path dir = make_scratch();
        {
            Messages m = make_messages(dir);
            m.add_system("sys");
            m.add_user("hi");
            m.add_user("with-src", "", "email:ops@example.com");
            m.add_assistant("reply");
            m.add_tool_result("call_1", "result");

            const json& chain = m.get_messages();
            REQUIRE(chain.size() == 5);
            for (const auto& msg : chain) {
                CAPTURE(msg.value("role", ""));
                REQUIRE(msg.contains("_meta"));
                CHECK(!msg["_meta"].value("received_at", "").empty());
            }
            CHECK(chain[0]["_meta"].value("source", "") == "system");
            CHECK(chain[1]["_meta"].value("source", "") == "user");
            CHECK(chain[2]["_meta"].value("source", "") == "email:ops@example.com");
            CHECK(chain[3]["_meta"].value("source", "") == "assistant");
            CHECK(chain[4]["_meta"].value("source", "") == "tool");
        }
        fs::remove_all(dir);
    }

    TEST_CASE("add_message (replay path) does not overwrite _meta") {
        const fs::path dir = make_scratch();
        {
            Messages m = make_messages(dir);
            json legacy = {{"role", "user"},
                           {"content", "old"},
                           {"_meta", {{"source", "user:lee"},
                                      {"received_at", "2026-09-01 10:00:00 AEST"}}}};
            m.add_message(legacy);
            const json& chain = m.get_messages();
            REQUIRE(chain.size() == 1);
            CHECK(chain[0]["_meta"].value("received_at", "") ==
                  "2026-09-01 10:00:00 AEST");
        }
        fs::remove_all(dir);
    }

    TEST_CASE("wire_render expands _meta into a content header and strips _meta") {
        json chain = json::array({
            {{"role", "system"}, {"content", "sys"},
             {"_meta", {{"source", "system"}, {"received_at", "2026-10-04 07:27:44 AEST"}}}},
            {{"role", "user"}, {"content", "do the thing"},
             {"_meta", {{"source", "trigger:nightly-backup"}, {"received_at", "2026-10-04 07:27:44 AEST"}}}},
        });

        const json wire = Messages::wire_render(chain);

        REQUIRE(wire.size() == 2);
        for (const auto& msg : wire) {
            CAPTURE(msg.value("role", ""));
            CHECK(!msg.contains("_meta"));  // strict providers never see it
        }
        CHECK(wire[0]["content"].get<std::string>().find(
                  "[from: system · received 2026-10-04 07:27:44 AEST]") == 0);
        CHECK(wire[1]["content"].get<std::string>().find(
                  "[from: trigger:nightly-backup · received 2026-10-04 07:27:44 AEST]") == 0);

        // The STORED chain is untouched — byte-stable across turns.
        CHECK(chain[1].contains("_meta"));
        CHECK(chain[1]["content"].get<std::string>() == "do the thing");
    }

    TEST_CASE("wire_render handles array (multimodal) content") {
        json chain = json::array({
            {{"role", "user"},
             {"content", json::array({
                 {{"type", "text"}, {"text", "what is this?"}},
                 {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,xx"}}}},
             })},
             {"_meta", {{"source", "user:lee"}, {"received_at", "2026-10-04 07:27:44 AEST"}}}},
        });

        const json wire = Messages::wire_render(chain);
        REQUIRE(wire.size() == 1);
        REQUIRE(wire[0]["content"].is_array());
        REQUIRE(wire[0]["content"].size() == 3);  // header + original 2 parts
        CHECK(wire[0]["content"][0].value("type", "") == "text");
        CHECK(wire[0]["content"][0].value("text", "").find("[from: user:lee") == 0);
        CHECK(wire[0]["content"][1].value("type", "") == "text");
        CHECK(wire[0]["content"][2].value("type", "") == "image_url");
    }

    TEST_CASE("wire_render passes legacy messages (no _meta) through unchanged") {
        json chain = json::array({
            {{"role", "user"}, {"content", "old message"}},
        });
        const json wire = Messages::wire_render(chain);
        REQUIRE(wire.size() == 1);
        CHECK(wire[0].value("content", "") == "old message");
        CHECK(!wire[0].contains("_meta"));
    }

    TEST_CASE("§ANTI-IMITATION: wire_render gives assistant messages no header") {
        json assistant = {{"role", "assistant"}, {"content", "plain reply"}};
        Messages::stamp(assistant, "assistant");
        json user = {{"role", "user"}, {"content", "hi"}};
        Messages::stamp(user, "user:lee");

        const json wire = Messages::wire_render(json::array({assistant, user}));

        // Assistant: _meta stripped (strict providers), content untouched.
        CHECK(!wire[0].contains("_meta"));
        CHECK(wire[0].value("content", "") == "plain reply");
        // User: header rendered as before.
        CHECK(wire[1].value("content", "").find("[from: user:lee") == 0);
        CHECK(!wire[1].contains("_meta"));
        // The stored copies still carry _meta — wire_render must not
        // mutate its input.
        CHECK(assistant.contains("_meta"));
        CHECK(user.contains("_meta"));
    }

    TEST_CASE("§ANTI-IMITATION: strip_leading_wire_header removes an imitated header") {
        json msg = {{"role", "assistant"},
                    {"content", "[from: assistant · received 2026-10-03 23:08:27 UTC]\n\n"
                                "The actual reply text."}};
        Messages::strip_leading_wire_header(msg);
        CHECK(msg.value("content", "") == "The actual reply text.");
    }

    TEST_CASE("§ANTI-IMITATION: strip leaves ordinary text and edge shapes alone") {
        // Mid-text mention, not a leading line: untouched.
        json quoted = {{"role", "assistant"},
                       {"content", "You asked about [from: something] earlier."}};
        Messages::strip_leading_wire_header(quoted);
        CHECK(quoted.value("content", "").find("You asked") == 0);

        // '[from:' that never closes on its first line: not a header.
        json unclosed = {{"role", "assistant"}, {"content", "[from: nowhere\nreal text"}};
        Messages::strip_leading_wire_header(unclosed);
        CHECK(unclosed.value("content", "") == "[from: nowhere\nreal text");

        // Header-only reply: keep the original rather than store empty.
        json only_header = {{"role", "assistant"},
                            {"content", "[from: assistant · received 2026-10-03 23:08:27 UTC]"}};
        Messages::strip_leading_wire_header(only_header);
        CHECK(only_header.value("content", "").find("[from: assistant") == 0);

        // Non-assistant roles are never stripped.
        json user = {{"role", "user"},
                     {"content", "[from: user:lee · received 2026-10-03 23:08:27 UTC]\n\nhi"}};
        Messages::strip_leading_wire_header(user);
        CHECK(user.value("content", "").find("[from: user:lee") == 0);
    }

    TEST_CASE("§ANTI-IMITATION: sanitize migrates a poisoned stored chain") {
        Messages m = make_messages(make_scratch());
        json poisoned = {
            {{"role", "system"}, {"content", "sys"}},
            {{"role", "user"}, {"content", "hello"}},
            {{"role", "assistant"},
             {"content", "[from: assistant · received 2026-10-03 23:08:27 UTC]\n\nanswer"}},
        };
        const json clean = m.sanitize_message_chain(poisoned);
        REQUIRE(clean.size() == 3);
        CHECK(clean[2].value("content", "") == "answer");
    }

    TEST_CASE("_meta survives a save/load round-trip") {
        const fs::path dir = make_scratch();
        {
            Messages m = make_messages(dir);
            m.add_user("persist me", "", "trigger:ci-pipeline");
            m.save();
        }
        {
            Messages m2 = make_messages(dir);
            REQUIRE(m2.load());
            const json& chain = m2.get_messages();
            REQUIRE(chain.size() == 1);
            REQUIRE(chain[0].contains("_meta"));
            CHECK(chain[0]["_meta"].value("source", "") == "trigger:ci-pipeline");
            CHECK(!chain[0]["_meta"].value("received_at", "").empty());
        }
        fs::remove_all(dir);
    }

    TEST_CASE("history_snapshot surfaces source and received_at") {
        const fs::path dir = make_scratch();
        {
            Messages m = make_messages(dir);
            m.add_system("sys");
            m.add_user("hi", "", "a2a:peer-agent");
            const auto snap = m.history_snapshot();
            REQUIRE(snap.size() == 2);
            CHECK(snap[0].source == "system");
            CHECK(!snap[0].received_at.empty());
            CHECK(snap[1].source == "a2a:peer-agent");
            CHECK(!snap[1].received_at.empty());
        }
        fs::remove_all(dir);
    }
}
