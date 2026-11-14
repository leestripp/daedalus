// SPDX-License-Identifier: BSL-1.0
// tests/test_pending_messages.cpp
//
// Pending queue management API: hosts can list, edit, remove and clear
// the user messages queued by Agent::submit_user_message() before
// run_one_turn() consumes them.
//
// Covers:
//   1. Empty queue — pending_messages() empty, edit/remove return false,
//      clear is a no-op.
//   2. pending_messages() snapshot — order (oldest first), text and
//      image attachments round-trip.
//   3. edit_pending_message() — text replace, images kept by default,
//      images replaced / cleared when a value is passed, out-of-range
//      index returns false and leaves the queue untouched.
//   4. remove_pending_message() — drops one entry, preserves the order
//      of the rest, out-of-range index returns false.
//   5. clear_pending_messages() — empties the queue.
//   6. §QUEUE-JOIN join-on-drain — run_one_turn() consumes the WHOLE
//      queue as ONE message: texts blank-line-joined FIFO, images
//      concatenated, sources merged; queue left empty.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <daedalus.h>

namespace {

// Minimal no-op sink that satisfies AgentSink's mandatory on_log.
struct NullSink : public daedalus::AgentSink {
    void on_log(daedalus::LogLevel, std::string_view) noexcept override {}
};

daedalus::Agent make_agent() {
    daedalus::AgentConfig cfg;
    cfg.project_root = std::filesystem::temp_directory_path()
                       / "daedalus_pending_msgs";
    return daedalus::Agent(cfg, std::make_shared<NullSink>());
}

daedalus::ImageAttachment make_image(std::string path) {
    daedalus::ImageAttachment img;
    img.source = daedalus::ImageAttachment::Source::Path;
    img.path_or_url = std::move(path);
    img.mime_type = "image/png";
    return img;
}

#define CHECK_MSG(cond)                                                     \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAILED (line " << __LINE__ << "): " #cond "\n";   \
            return 1;                                                       \
        }                                                                   \
    } while (0)

int run() {
    // ---- 1. Empty queue --------------------------------------------------
    {
        auto agent = make_agent();
        CHECK_MSG(agent.pending_count() == 0);
        CHECK_MSG(agent.pending_messages().empty());
        CHECK_MSG(!agent.edit_pending_message(0, "nope"));
        CHECK_MSG(!agent.remove_pending_message(0));
        agent.clear_pending_messages();  // no-op, must not crash
        CHECK_MSG(agent.pending_count() == 0);
    }
    std::cout << "[1] empty queue OK\n";

    // ---- 2. Snapshot order + round-trip ----------------------------------
    {
        auto agent = make_agent();
        agent.submit_user_message("first");
        agent.submit_user_message("second", {make_image("/tmp/a.png")});
        agent.submit_user_message("third");

        CHECK_MSG(agent.pending_count() == 3);
        const auto msgs = agent.pending_messages();
        CHECK_MSG(msgs.size() == 3);
        CHECK_MSG(msgs[0].text == "first");
        CHECK_MSG(msgs[0].images.empty());
        CHECK_MSG(msgs[1].text == "second");
        CHECK_MSG(msgs[1].images.size() == 1);
        CHECK_MSG(msgs[1].images[0].path_or_url == "/tmp/a.png");
        CHECK_MSG(msgs[2].text == "third");

        // Snapshot is a copy — mutating it must not touch the queue.
        auto mut = agent.pending_messages();
        mut[0].text = "mutated";
        CHECK_MSG(agent.pending_messages()[0].text == "first");
    }
    std::cout << "[2] snapshot OK\n";

    // ---- 3. Edit ----------------------------------------------------------
    {
        auto agent = make_agent();
        agent.submit_user_message("first");
        agent.submit_user_message("second", {make_image("/tmp/a.png")});

        // Text-only edit keeps attachments.
        CHECK_MSG(agent.edit_pending_message(1, "second v2"));
        auto msgs = agent.pending_messages();
        CHECK_MSG(msgs[1].text == "second v2");
        CHECK_MSG(msgs[1].images.size() == 1);

        // Explicit images replace.
        CHECK_MSG(agent.edit_pending_message(
            1, "second v3",
            std::vector<daedalus::ImageAttachment>{make_image("/tmp/b.png")}));
        msgs = agent.pending_messages();
        CHECK_MSG(msgs[1].text == "second v3");
        CHECK_MSG(msgs[1].images.size() == 1);
        CHECK_MSG(msgs[1].images[0].path_or_url == "/tmp/b.png");

        // Empty vector clears attachments.
        CHECK_MSG(agent.edit_pending_message(
            1, "second v4", std::vector<daedalus::ImageAttachment>{}));
        CHECK_MSG(agent.pending_messages()[1].images.empty());

        // Out of range: false, queue untouched.
        CHECK_MSG(!agent.edit_pending_message(2, "nope"));
        CHECK_MSG(agent.pending_count() == 2);
        CHECK_MSG(agent.pending_messages()[0].text == "first");
    }
    std::cout << "[3] edit OK\n";

    // ---- 4. Remove --------------------------------------------------------
    {
        auto agent = make_agent();
        agent.submit_user_message("a");
        agent.submit_user_message("b");
        agent.submit_user_message("c");

        CHECK_MSG(agent.remove_pending_message(1));  // drop "b"
        const auto msgs = agent.pending_messages();
        CHECK_MSG(msgs.size() == 2);
        CHECK_MSG(msgs[0].text == "a");
        CHECK_MSG(msgs[1].text == "c");

        CHECK_MSG(!agent.remove_pending_message(2));  // out of range
        CHECK_MSG(agent.pending_count() == 2);

        CHECK_MSG(agent.remove_pending_message(0));  // drop head
        CHECK_MSG(agent.pending_count() == 1);
        CHECK_MSG(agent.pending_messages()[0].text == "c");
    }
    std::cout << "[4] remove OK\n";

    // ---- 5. Clear ---------------------------------------------------------
    {
        auto agent = make_agent();
        agent.submit_user_message("a");
        agent.submit_user_message("b");
        agent.clear_pending_messages();
        CHECK_MSG(agent.pending_count() == 0);
        CHECK_MSG(agent.pending_messages().empty());
        agent.clear_pending_messages();  // idempotent
    }
    std::cout << "[5] clear OK\n";

    // ---- 6. Join-on-drain (§QUEUE-JOIN) ----------------------------------
    // run_one_turn() consumes the ENTIRE queue as one joined message —
    // observable afterwards in history_snapshot() as a single user entry
    // whose content is the FIFO blank-line join. DAEDALUS_HOME is pointed
    // at an empty dir so the test is hermetic (no user-tier providers
    // from the real store) and fast (no live network call to a configured
    // endpoint).
    {
        const std::string isolated_home =
            (std::filesystem::temp_directory_path() / "daedalus_qj_home")
                .string();
        std::filesystem::create_directories(isolated_home);
        ::setenv("DAEDALUS_HOME", isolated_home.c_str(), 1);

        // Fresh project root — a persisted chain from an earlier run of
        // this binary would otherwise add stale user entries below.
        const std::filesystem::path qj_root =
            std::filesystem::temp_directory_path() / "daedalus_qj_proj";
        std::filesystem::remove_all(qj_root);
        daedalus::AgentConfig qj_cfg;
        qj_cfg.project_root = qj_root;
        daedalus::Agent agent(qj_cfg, std::make_shared<NullSink>());

        agent.submit_user_message("do X");
        agent.submit_user_message("also change Y", {make_image("/tmp/a.png")});
        agent.submit_user_message("correction: actually Z", {},
                                  "user:lee");

        agent.run_one_turn();

        // Whole queue consumed by the single turn.
        CHECK_MSG(agent.pending_count() == 0);
        CHECK_MSG(agent.pending_messages().empty());

        // Joined text is dispatched as ONE user message (FIFO, blank
        // line between parts) — observable in the history snapshot.
        const auto hist = agent.history_snapshot();
        size_t user_entries = 0;
        std::string user_content;
        for (const auto& h : hist) {
            if (h.role == "user") {
                ++user_entries;
                user_content = h.content;
            }
        }
        CHECK_MSG(user_entries == 1);
        CHECK_MSG(user_content ==
                  "do X\n\nalso change Y\n\ncorrection: actually Z");

        ::unsetenv("DAEDALUS_HOME");
    }
    std::cout << "[6] join-on-drain OK\n";

    // ---- 7. Idle Steer falls back to the ordinary pending queue ---------
    {
        auto agent = make_agent();
        agent.submit_steer_message("idle steering", {}, "steer:lee");
        CHECK_MSG(agent.pending_count() == 1);
        const auto pending = agent.pending_messages();
        CHECK_MSG(pending.size() == 1);
        CHECK_MSG(pending[0].text == "idle steering");
        CHECK_MSG(pending[0].source == "steer:lee");
    }
    std::cout << "[7] idle Steer fallback OK\n";

    return 0;
}

}  // namespace

int main() {
    const int rc = run();
    if (rc == 0) {
        std::cout << "All pending-message tests passed.\n";
    }
    return rc;
}
