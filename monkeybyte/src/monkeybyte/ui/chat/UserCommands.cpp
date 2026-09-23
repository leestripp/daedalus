#include "monkeybyte/ui/chat/UserCommands.h"

#include <algorithm>
#include <cctype>

namespace monkeybyte {

const std::vector<UserCommand>& user_commands() {
    static const std::vector<UserCommand> kCommands = {
        {"init", "Create or update AGENTS.md for this workspace"},
        {"compress", "Request context compression (daedalus summarizes the oldest ~70%)"},
        {"clear", "Clear the chat history and reset the agent session"},
        {"usage", "Show token and context usage for this session"},
        {"tools", "List the agent's registered tools"},
        {"skills", "List loaded skills"},
        {"mcp", "List configured MCP servers"},
        {"help", "List available commands"},
    };
    return kCommands;
}

namespace {

std::string_view trim(std::string_view s) {
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

} // namespace

bool looks_like_command(std::string_view text) {
    return trim(text).starts_with('/');
}

std::optional<std::string> parse_command(std::string_view text) {
    const std::string_view t = trim(text);
    if (t.size() < 2 || t.front() != '/') return std::nullopt;
    const std::string_view name = t.substr(1);
    // v1 commands take no arguments.
    if (name.find_first_of(" \t\n\r") != std::string_view::npos) return std::nullopt;
    const auto& cmds = user_commands();
    const auto it = std::find_if(cmds.begin(), cmds.end(),
                                 [&](const UserCommand& c) { return c.name == name; });
    if (it == cmds.end()) return std::nullopt;
    return it->name;
}

} // namespace monkeybyte
