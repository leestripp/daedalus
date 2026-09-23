#pragma once

#include "monkeybyte/backend/Logger.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace monkeybyte {

// Run a callable at a GTK callback boundary (signal handler, timeout/idle
// source, Gio action, C-library callback, worker-thread body). GLib aborts
// the process when a C++ exception escapes such a callback, so catch
// everything, log it (§7.2), and degrade to a no-op instead of crashing.
template <typename F> void ui_guard(std::string_view context, F&& fn) noexcept {
    try {
        fn();
    } catch (const std::exception& e) {
        Logger::instance().error(std::string(context) + ": " + e.what());
    } catch (...) {
        Logger::instance().error(std::string(context) + ": unknown exception");
    }
}

} // namespace monkeybyte
