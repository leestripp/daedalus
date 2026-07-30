// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/SettingsApp.cpp

#include "SettingsApp.h"

#include <ftxui/component/app.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <charconv>
#include <cstdlib>
#include <map>
#include <ostream>
#include <sstream>

namespace settings_tui {

using namespace ftxui;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string MaskSecret(const std::string& secret)
{
    if (secret.empty()) return "(not set)";
    if (secret.size() <= 4) return "****";
    return secret.substr(0, 3) + "…" + secret.substr(secret.size() - 4);
}

bool ParseInt(const std::string& s, int& out)
{
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

bool ParseSize(const std::string& s, size_t& out)
{
    if (s.empty()) return false;
    unsigned long long tmp = 0;
    const char* first = s.data();
    const char* last  = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, tmp);
    if (ec != std::errc{} || ptr != last) return false;
    out = static_cast<size_t>(tmp);
    return true;
}

bool ParseDouble(const std::string& s, double& out)
{
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size();
}

std::vector<std::string> SplitWords(const std::string& s)
{
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string word;
    while (ss >> word) out.push_back(word);
    return out;
}

std::string JoinWords(const std::vector<std::string>& words)
{
    std::string out;
    for (const auto& w : words) {
        if (!out.empty()) out += ' ';
        out += w;
    }
    return out;
}

std::map<std::string, std::string> ParseEnvLines(const std::string& s)
{
    std::map<std::string, std::string> out;
    std::istringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

std::string EnvToLines(const std::map<std::string, std::string>& env)
{
    std::string out;
    for (const auto& [k, v] : env) {
        out += k + "=" + v + "\n";
    }
    return out;
}

Component FormRow(const std::string& label,
                  std::string* content,
                  bool password,
                  std::function<void()> on_enter)
{
    InputOption opt;
    opt.content   = StringRef(content);
    opt.password  = password;
    opt.multiline = false;
    if (on_enter) opt.on_enter = std::move(on_enter);
    auto input = Input(opt);
    return Renderer(input, [label, input] {
        return hbox({
            text(label) | size(WIDTH, EQUAL, 24),
            input->Render() | flex,
        });
    });
}

// ---------------------------------------------------------------------------
// Interactive app
// ---------------------------------------------------------------------------

int RunApp(daedalus::SettingsStore store)
{
    AppContext ctx{std::move(store), {}, {}, {}};

    static const std::vector<std::string> kTitles = {
        "Providers", "Models", "MCP Servers", "Agent",
    };

    int tab_index = 0;
    auto toggle = Toggle(&kTitles, &tab_index);
    auto tabs   = Container::Tab(
        {
            MakeProvidersTab(ctx),
            MakeModelsTab(ctx),
            MakeMcpTab(ctx),
            MakeAgentTab(ctx),
        },
        &tab_index);

    auto app  = App::Fullscreen();
    // Deliver Ctrl+C as an event (handled below) instead of letting the
    // default handler kill the process with SIGINT.
    app.ForceHandleCtrlC(false);
    auto quit = Button(" Quit ", app.ExitLoopClosure());

    // Focus stays on the tab content (selector = 1): the header toggle
    // is mouse-driven, keyboard users switch tabs with the Left/Right
    // arrows handled at the root below.
    int main_selector = 1;
    auto header = Container::Horizontal({toggle, quit});
    auto main   = Container::Vertical({header, tabs}, &main_selector);

    auto renderer = Renderer(main, [&] {
        return vbox({
            text(" daedalus settings — " + ctx.store.db_path().string()) | bold,
            separator(),
            hbox({
                toggle->Render() | flex,
                quit->Render(),
            }),
            separator(),
            tabs->Render() | flex,
            separator(),
            hbox({
                text(" " + ctx.status) | color(Color::Yellow) | flex,
                text("←/→: switch section · Ctrl+C: quit ") | dim,
            }),
        });
    });

    // Graceful Ctrl+C (exit code 0, no SIGINT death), Left/Right tab
    // switching, and routing of tab shortcut keys to the active tab's
    // registered handler. While a modal dialog is open, navigation keys
    // are left for the modal itself.
    auto with_quit = renderer | CatchEvent([&](Event e) {
        if (e == Event::CtrlC) {
            app.Exit();
            return true;
        }
        const int idx = tab_index;
        const bool modal_open =
            idx >= 0 && idx < static_cast<int>(ctx.modal_checks.size()) &&
            ctx.modal_checks[idx]();
        if (modal_open) return false;

        if (e == Event::ArrowLeft && idx > 0) {
            --tab_index;
            return true;
        }
        if (e == Event::ArrowRight &&
            idx < static_cast<int>(kTitles.size()) - 1) {
            ++tab_index;
            return true;
        }
        if (idx >= 0 && idx < static_cast<int>(ctx.key_handlers.size()) &&
            ctx.key_handlers[idx](e)) {
            return true;
        }
        return false;
    });

    app.Loop(with_quit);
    return 0;
}

// ---------------------------------------------------------------------------
// Non-interactive dump
// ---------------------------------------------------------------------------

int DumpSettings(daedalus::SettingsStore& store, std::ostream& out)
{
    out << "db: " << store.db_path().string() << "\n";

    out << "\n[providers]\n";
    const auto providers = store.list_providers_detailed();
    if (providers.empty()) out << "  (none)\n";
    for (const auto& p : providers) {
        out << "  " << p.name << "\n"
            << "    host               = " << p.host << "\n"
            << "    api_key            = " << MaskSecret(p.api_key) << "\n"
            << "    chat_endpoint      = " << p.chat_endpoint << "\n"
            << "    discovery_endpoint = " << p.discovery_endpoint << "\n"
            << "    timeout_seconds    = " << p.timeout_seconds << "\n";
    }

    out << "\n[models]\n";
    const auto models          = store.list_models();
    const std::string def_name = store.get_default_model_name();
    if (models.empty()) out << "  (none)\n";
    for (const auto& m : models) {
        const bool is_default = (m.name == def_name && !def_name.empty());
        out << (is_default ? "* " : "  ") << m.name << "\n"
            << "    provider         = " << m.provider << "\n"
            << "    model_id         = " << m.model_id << "\n"
            << "    max_context_size = " << m.max_context_size << "\n"
            << "    temperature      = " << m.temperature << "\n";
    }
    if (def_name.empty()) out << "  (no default model set)\n";

    out << "\n[mcp_servers]\n";
    const auto servers = store.list_mcp_servers();
    if (servers.empty()) out << "  (none)\n";
    for (const auto& s : servers) {
        out << "  " << s.name << " [" << (s.enabled ? "enabled" : "disabled") << "]\n"
            << "    transport    = " << s.transport << "\n";
        if (s.transport == "stdio") {
            out << "    command      = " << s.command << "\n"
                << "    args         = " << JoinWords(s.args) << "\n";
            for (const auto& [k, v] : s.env)
                out << "    env." << k << " = " << MaskSecret(v) << "\n";
        } else {
            out << "    url          = " << s.url << "\n"
                << "    bearer_token = " << MaskSecret(s.bearer_token) << "\n";
        }
        if (!s.description.empty())
            out << "    description  = " << s.description << "\n";
    }

    const auto ses = store.get_server_error_settings();
    out << "\n[agent]\n"
        << "  default_model_name               = " << (def_name.empty() ? "(none)" : def_name) << "\n"
        << "  current_provider                 = " << store.get_current_provider() << "\n"
        << "  auto_approve                     = " << (store.get_auto_approve() ? "true" : "false") << "\n"
        << "  max_agent_iterations             = " << store.get_max_agent_iterations() << "\n"
        << "  max_tool_result_size             = " << store.get_max_tool_result_size() << "\n"
        << "  max_request_size                 = " << store.get_max_request_size() << "\n"
        << "  max_response_size                = " << store.get_max_response_size() << "\n"
        << "  mcp_tool_schema_soft_cap_percent = " << store.get_mcp_tool_schema_soft_cap_percent() << "\n"
        << "  serper_api_key                   = " << MaskSecret(store.get_serper_api_key()) << "\n"
        << "\n[server_error]\n"
        << "  enabled               = " << (ses.enabled ? "true" : "false") << "\n"
        << "  initial_wait_seconds  = " << ses.initial_wait_seconds << "\n"
        << "  max_wait_seconds      = " << ses.max_wait_seconds << "\n"
        << "  backoff_multiplier    = " << ses.backoff_multiplier << "\n"
        << "  max_retries_per_call  = " << ses.max_retries_per_call << "\n";

    return 0;
}

} // namespace settings_tui
