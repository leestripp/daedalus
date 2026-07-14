/**
 * @file LogLevel.cpp
 * @brief Implementation of daedalus::to_string(LogLevel).
 */
#include "daedalus/types/LogLevel.h"

namespace daedalus {

const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "Trace";
        case LogLevel::Debug: return "Debug";
        case LogLevel::Info:  return "Info";
        case LogLevel::Warn:  return "Warn";
        case LogLevel::Error: return "Error";
        case LogLevel::Fatal: return "Fatal";
    }
    return "Unknown";
}

}  // namespace daedalus