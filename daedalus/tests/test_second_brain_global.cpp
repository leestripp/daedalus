// SPDX-License-Identifier: BSL-1.0
// Tests for the process-wide Second Brain's configurable Markdown root.

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <daedalus/paths.h>
#include <daedalus/settings.h>

#include "core/SecondBrain.h"

namespace fs = std::filesystem;
using namespace daedalus;

namespace {
struct TmpDir {
    fs::path path;
    TmpDir() {
        path = fs::temp_directory_path() /
               ("daedalus-brain-global-" + std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TmpDir() { std::error_code ec; fs::remove_all(path, ec); }
};
}  // namespace

int main() {
    TmpDir tmp;
    // Must precede the first Paths::global()/SettingsStore::instance() call.
    // Set the data-tier override explicitly: DAEDALUS_HOME is the legacy
    // user_root umbrella and does not override XDG_DATA_HOME for this tier.
    const fs::path global_home = tmp.path / "global-home";
    setenv("DAEDALUS_DATA_HOME", global_home.c_str(), 1);

    auto& store = SettingsStore::instance();
    const fs::path expected_default =
        Paths::global().user_data_root() / "brain";
    assert(store.get_second_brain_directory() == expected_default);
    assert(SecondBrain::global().root() == expected_default);

    const fs::path mounted = tmp.path / "workspace" / "mounted-brain";
    store.set_second_brain_directory(mounted);
    auto& first = SecondBrain::global();
    assert(first.root() == mounted);
    const auto note = first.capture("resources", "Mounted Note", {},
                                    "This note lives on the mounted drive.");
    assert(fs::exists(mounted / "resources" / "mounted-note.md"));
    assert(first.read(note.id).has_value());

    // Switching roots selects a different KB and reconciles the shared index,
    // so notes from the previous root do not leak into list/search results.
    const fs::path other = tmp.path / "other-brain";
    store.set_second_brain_directory(other);
    auto& second = SecondBrain::global();
    assert(second.root() == other);
    assert(!second.read(note.id).has_value());
    assert(second.list().empty());

    // Switching back does not move/copy anything; the original note is still
    // on the mounted drive and is indexed again on first selection.
    store.set_second_brain_directory(mounted);
    auto& restored = SecondBrain::global();
    assert(&restored == &first);
    assert(restored.read(note.id).has_value());
    assert(!fs::exists(other / "resources" / "mounted-note.md"));

    std::cout << "Second Brain global directory configuration passed\n";
    return 0;
}
