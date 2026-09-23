#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace monkeybyte {

// §7.8 — in-session user commands ("/init", "/compress", …), inspired by
// the icarus CLI's in-session commands. This registry is GTK-free so the
// parser is unit-testable; execution lives in ChatPanel::run_command.
struct UserCommand {
    std::string name;        // without the leading '/'
    std::string description;
};

// All supported commands, in menu display order.
const std::vector<UserCommand>& user_commands();

// Returns the command name if `text` (trimmed) is exactly "/name" for a
// registered command. Commands take no arguments, so "/cmd extra" and
// unknown "/foo" both return nullopt.
std::optional<std::string> parse_command(std::string_view text);

// True if `text` (trimmed) starts with '/' — used to warn about unknown
// commands instead of sending them to the agent.
bool looks_like_command(std::string_view text);

} // namespace monkeybyte
