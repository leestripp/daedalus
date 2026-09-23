#include "monkeybyte/backend/Logger.h"

#include <chrono>
#include <cstdlib>
#include <string>

namespace monkeybyte {

namespace {

// Same base dir as monkeybyte.db (design §10.2).
std::filesystem::path default_log_path() {
    std::filesystem::path base;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = std::filesystem::path(home) / ".local" / "share";
    else
        base = std::filesystem::temp_directory_path();
    return base / "monkeybyte" / "monkeybyte.log";
}

} // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() : path_(default_log_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path_.parent_path(), ec);
    out_.open(path_, std::ios::app);
}

void Logger::error(std::string_view message) {
    std::string line(message);
    for (char& c : line)
        if (c == '\n' || c == '\r') c = ' ';

    const auto now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_r(&now, &tm);
    char ts[20];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    std::lock_guard lock(mutex_);
    if (out_) out_ << ts << " [ERROR] " << line << '\n' << std::flush;
}

} // namespace monkeybyte
