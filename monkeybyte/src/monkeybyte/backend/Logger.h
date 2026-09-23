#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace monkeybyte {

// §7.2 — minimal append-only error log so agent/API failures can be
// diagnosed after the fact, not just seen in the chat banner. Writes one
// timestamped line per entry to
// $XDG_DATA_HOME/monkeybyte/monkeybyte.log (~/.local/share fallback).
// Thread-safe, GTK-free.
class Logger {
public:
    static Logger& instance();

    // Appends "2026-07-28 01:24:00 [ERROR] <message>" (newlines in the
    // message are flattened so each entry stays on one line).
    void error(std::string_view message);

    const std::filesystem::path& path() const { return path_; }

private:
    Logger();

    std::filesystem::path path_;
    std::ofstream out_;
    std::mutex mutex_;
};

} // namespace monkeybyte
