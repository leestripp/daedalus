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
