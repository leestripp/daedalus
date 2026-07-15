/**
 * @file AgentImpl.cpp
 *
 * §Phase 1+: implements daedalus::Agent by wrapping the real subsystems.
 *
 * Wiring:
 *   AgentConfig ──> AppConfig ──> LLMClient
 *   LLMClient ──> AgentSink (already wired via LLMClient::set_sink)
 *   LLMClient::AgentState ──> daedalus::AgentState (map_from_internal)
 *   cancel() ──> LLMClient internal stop_token
 */
#include "AgentImpl.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <stop_token>
#include <utility>

#include "daedalus/paths.h"
#include "daedalus/settings.h"
#include "api/LLMClient.h"           // internal subsystem (src/api/)
#include "core/SessionMemory.h"      // internal subsystem (src/core/)
#include "core/SkillManager.h"       // internal subsystem (src/core/)
#include "tools/ToolHandler.h"       // internal subsystem (src/tools/)
#include "tools/ToolFactory.h"       // internal subsystem (src/tools/)
#include "tools/HostToolAdapter.h"   // adapter: HostTool → BaseTool
#include "LibraryToolContext.h"
#include "daedalus/default_paths.h"   // default_settings_path()

namespace daedalus::detail {

namespace fs = std::filesystem;

// ===========================================================================
// Body — holds LLMClient. (Defined in AgentImpl.h so that
// std::unique_ptr<Body>'s destructor is visible at the AgentImpl dtor site.)
// ===========================================================================

// ---------------------------------------------------------------------------
// State mapping
// ---------------------------------------------------------------------------

namespace {

constexpr AgentState map_from_internal(::AgentState s) noexcept {
    switch (s) {
        case ::AgentState::UNDERSTAND:  return AgentState::Understanding;
        case ::AgentState::PLANNING:    return AgentState::Planning;
        case ::AgentState::ACT:         return AgentState::Acting;
        case ::AgentState::BUILD_TEST:  return AgentState::Verifying;
        case ::AgentState::DONE:        return AgentState::Done;
        case ::AgentState::CANCELLED:   return AgentState::Cancelled;
    }
    return AgentState::Error;
}

AppConfig make_app_config(const AgentConfig& cfg,
                          daedalus::SettingsStore* store)
{
    AppConfig out;
    out.target_directory = cfg.project_root.empty()
                            ? std::filesystem::current_path()
                            : cfg.project_root;
    out.agent_name = cfg.agent_name;        // §Phase 8.8
    if (store && !cfg.model_name.empty()) {
        auto m = store->get_model(cfg.model_name);
        if (m.has_value()) {
            out.base_url      = m->host;
            out.api_key       = m->api_key;
            out.model         = m->model_id;
            out.model_name    = cfg.model_name;     // settings key
            out.chat_endpoint = m->chat_endpoint;
            out.temperature   = m->temperature;  // §Phase 8.7
        }
    }
    // Fall back to explicit AgentConfig fields if settings didn't supply.
    if (out.base_url.empty()) out.base_url = cfg.api_host;
    if (out.api_key.empty())  out.api_key  = cfg.api_key;
    if (out.model.empty())    out.model    = cfg.model_name;
    if (out.model_name.empty()) out.model_name = cfg.model_name;
    if (out.chat_endpoint.empty())
        out.chat_endpoint = cfg.chat_endpoint.empty()
                                ? "/v1/chat/completions" : cfg.chat_endpoint;

    out.auto_mode      = cfg.auto_approve;
    out.memory_disabled = !cfg.memory_enabled;
    out.system_prompt  = cfg.system_prompt;   // §Phase 6: host-supplied role
    out.stream         = cfg.stream;            // §Phase 9.8: SSE streaming
    // §Phase 8.7: temperature resolution. Explicit cfg.temperature >= 0
    // wins over the model's value. Negative = use model's or default 0.0.
    if (cfg.temperature >= 0.0) {
        out.temperature = cfg.temperature;
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// AgentImpl
// ---------------------------------------------------------------------------

AgentImpl::AgentImpl(AgentConfig cfg, std::shared_ptr<AgentSink> sink)
    : cfg_(std::move(cfg))
    , sink_(std::move(sink))
    , body_(std::make_unique<Body>())
    , host_ctx_(std::make_shared<LibraryToolContext>(cfg_))
{
    // §Phase 8.5: resolve derived paths from `cfg.daedalus_home` if those
    // path fields are empty. Priority (highest first):
    //   1. cfg.settings_path etc. (explicit per-file)
    //   2. cfg.daedalus_home (per-Agent root)
    //   3. default_settings_path() (Plan.md §3.7 — honours
    //      $DAEDALUS_HOME, $XDG_CONFIG_HOME, legacy $HOME/.daedalus)
    if (cfg_.settings_path.empty()) {
        if (!cfg_.daedalus_home.empty()) {
            cfg_.settings_path = cfg_.daedalus_home / "config.json";
        } else {
            cfg_.settings_path = default_settings_path();
        }
    }
    if (cfg_.mcp_servers_path.empty() && !cfg_.daedalus_home.empty()) {
        cfg_.mcp_servers_path = cfg_.daedalus_home / "mcp" / "servers.json";
    }
    if (cfg_.mcp_env_path.empty() && !cfg_.daedalus_home.empty()) {
        cfg_.mcp_env_path = cfg_.daedalus_home / "mcp" / ".env";
    }
    if (cfg_.memory_dir.empty() && !cfg_.daedalus_home.empty()) {
        cfg_.memory_dir = cfg_.daedalus_home / "memory";
    }

    // §v0.5.0: wire directly to SettingsStore instead of SettingsManager.
    // Use per-agent DB if daedalus_home is set, otherwise the global singleton.
    if (!cfg_.daedalus_home.empty()) {
        body_->store = &daedalus::SettingsStore::for_home(cfg_.daedalus_home);
    } else {
        body_->store = &daedalus::SettingsStore::instance();
    }
}

void AgentImpl::set_state(AgentState s) noexcept
{
    AgentState old = state_.exchange(s, std::memory_order_acq_rel);
    if (old != s && sink_) {
        sink_->on_state_change(static_cast<int>(old), static_cast<int>(s));
        // §Phase 9.6: notify the host on turn completion. The CLI rings
        // the terminal bell, the webui plays a sound / flashes the
        // title — whatever the host's "attention please" gesture is.
        // Default no-op stays correct for hosts that don't care.
        if (s == AgentState::Done) {
            sink_->on_bell();
        }
    }
}

void AgentImpl::initialize()
{
    if (initialized_) return;

    // §Phase 14.7: per-project system_prompt override.
    // Decision (see Agent.md §14.7 Q2): if
    // <project_root>/.daedalus/system_prompt.txt exists, its contents
    // are APPENDED to cfg_.system_prompt after the demo's default.
    // Demo default is the base; project file is concatenated after.
    // If cfg_.system_prompt is empty, only the project file is used.
    {
        std::error_code ec;
        fs::path override_path = cfg_.project_root / ".daedalus" / "system_prompt.txt";
        if (fs::is_regular_file(override_path, ec)) {
            std::ifstream f(override_path);
            if (f) {
                std::ostringstream ss;
                ss << f.rdbuf();
                std::string project_text = ss.str();
                if (!project_text.empty()) {
                    if (cfg_.system_prompt.empty()) {
                        cfg_.system_prompt = std::move(project_text);
                    } else {
                        cfg_.system_prompt += "\n\n" + project_text;
                    }
                    if (sink_) sink_->on_log(daedalus::LogLevel::Info,
                        "[SYSTEM_PROMPT] Merged project override: " +
                        override_path.string());
                }
            }
        }
    }

    // §Phase 8.8: validate agent_name up-front so any later
    // Messages / SessionMemory init fails loudly with a clear
    // message rather than silently producing a bad path.
    if (!is_valid_agent_name(cfg_.agent_name)) {
        throw std::invalid_argument(
            "AgentConfig::agent_name is invalid: \"" + cfg_.agent_name +
            "\". Phase 8.8 rule: must match [A-Za-z0-9_.-]{1,64} and "
            "must not start with '.' or '-'. Empty string is allowed "
            "(means 'no namespace, use <project_root>/.daedalus/').");
    }

    // §Plan.md Phase 4 (2026-07-13): deprecation warning for legacy
    // ~/.daedalus/ user-tier layout. Per Plan.md §3.4, the library
    // honours the legacy path as a READ fallback for one release
    // cycle, but warns once per process so users notice.
    //
    // Detection rule (must match Paths::user_root() in src/library/paths.cpp):
    //   - $DAEDALUS_HOME unset (override wins, no deprecation)
    //   - $XDG_DATA_HOME unset  (XDG wins, no deprecation)
    //   - $HOME/.daedalus exists on disk → legacy layout in use
    //
    // When legacy layout is detected, emit one Warn via the sink so
    // the user sees it in their terminal. We do NOT print to stderr
    // (no-stdio rule, see Agent.md §Phase 5.1).
    {
        const char* home = std::getenv("HOME");
        const char* xdg_data = std::getenv("XDG_DATA_HOME");
        const char* daedalus_home = std::getenv("DAEDALUS_HOME");
        std::error_code ec;
        bool legacy_in_use = !daedalus_home && !xdg_data
                             && home && *home
                             && std::filesystem::is_directory(
                                 std::filesystem::path(home) / ".daedalus", ec);
        if (legacy_in_use && sink_) {
            sink_->on_log(daedalus::LogLevel::Warn,
                "[DEPRECATION] ~/.daedalus/ is the legacy user-tier layout. "
                "Per Plan.md §3.4 it is honoured as a READ fallback for one "
                "release cycle, then removed. Migrate via "
                "`daedalus-cli --migrate-data`, or set $DAEDALUS_HOME / "
                "$XDG_CONFIG_HOME / $XDG_DATA_HOME to opt into XDG-compliant "
                "locations immediately. See docs/storage.md (Plan.md Phase 6).");
        }
    }

    // §Plan.md §5.1: ensure the four XDG user-tier directories exist
    // with the right permissions BEFORE any subsystem writes to them.
    // This is the "auto-create on first write" half of Open Question #2
    // (the read side stays lazy — see Plan.md §5.2 Q2 for the rationale).
    //
    // Putting the call here (start of AgentImpl::initialize()) means:
    //   - Per-`Agent` instantiation, the dirs exist exactly once per
    //     process (because Paths::global() is the singleton).
    //   - Subsystems that write to mcp/.env / config.json / daedalus.log
    //     downstream don't have to think about mkdir + chmod.
    //   - On legacy ~/.daedalus/ installs this also chmods the legacy
    //     dir to 0750 — a one-time tightening for users who haven't
    //     migrated yet.
    Paths::global().ensure_directories();

    // §Phase 1+: only spin up the LLMClient if we have something to talk to.
    // "Explicit creds" now includes three sources (highest first):
    //   1. cfg_.model_name set                (e.g. --model minimax-m3)
    //   2. cfg_.api_key + cfg_.api_host set   (full override from CLI args)
    //   3. settings store has a default_model  (resolved below)
    //
    // Phase 8.5 (webui work): it was previously easy to ship the Agent
    // with no model at all if the host forgot to pass --model — the
    // settings default wasn't treated as a credential source.
    // The webui demo is exactly such a host: it knows nothing about
    // models and just passes cfg.daedalus_home. Now those hosts work
    // out of the box too.
    bool has_explicit_creds = !cfg_.model_name.empty()
        || (!cfg_.api_key.empty() && !cfg_.api_host.empty());
    if (!has_explicit_creds && body_->store) {
        try {
            auto default_model = body_->store->get_default_model_name();
            if (!default_model.empty()
                && body_->store->get_model(default_model).has_value()) {
                cfg_.model_name = default_model;
                has_explicit_creds = true;
            }
        } catch (...) { /* not fatal */ }
    }

    // §Phase 1+: honour cfg.clear_memory — wipe the project's session memory
    // AND the conversation-history file before loading them. Hosts can do this
    // once via the CLI flag; subsequent sessions start fresh.
    //
    // §Phase 8.5+: also wipe when --no-memory is set. The user-facing
    // semantics of "no memory" is "no prior context of any kind", which
    // includes the conversation history (`<project>/.daedalus/messages.json`),
    // not just the SessionMemory notes. Without this, a long session
    // (e.g. 1000+ messages) would exceed cheaper models' context windows
    // (e.g. MiniMax-M2.7 at 200K tokens) and the API would 400.
    if (cfg_.clear_memory || !cfg_.memory_enabled) {
        // Wipe the conversation-history file. SessionMemory::clear_all_memories
        // only covers the auxiliary notes; the message log is a separate file.
        try {
            // §Phase 8.8: target the per-agent namespace dir.
            fs::path messages_file = resolve_agent_dir(cfg_.project_root, cfg_.agent_name)
                                     / "messages.json";
            if (fs::exists(messages_file)) {
                fs::remove(messages_file);
                if (sink_) {
                    sink_->on_log(daedalus::LogLevel::Info,
                                  "[MEMORY] Conversation history wiped: " +
                                  messages_file.string());
                }
            }
        } catch (const std::exception& e) {
            if (sink_) {
                sink_->on_log(daedalus::LogLevel::Warn,
                              std::string("[MEMORY] Could not wipe messages.json: ") +
                              e.what());
            }
        }
    }

    if (cfg_.clear_memory) {
        // §Phase 8.8: clear memory lives under the per-agent
        // namespace so a named agent doesn't wipe its own peers.
        SessionMemory mem(resolve_agent_dir(cfg_.project_root, cfg_.agent_name));
        mem.clear_all_memories();
        if (sink_) {
            sink_->on_log(daedalus::LogLevel::Info,
                          "[MEMORY] All session memory cleared for this project");
        }
        // The flag is one-shot — clear it so re-initialize() doesn't repeat.
        cfg_.clear_memory = false;
    }

    if (has_explicit_creds) {
        AppConfig app = make_app_config(cfg_, body_->store);

        // §Phase 11: construct McpServerManager if mcp_enabled (default true).
        // Pass the raw pointer into LLMClient — it replaces all ::instance()
        // call sites in the library. When mcp_enabled=false, body_->mcp is
        // nullptr and LLMClient's mcp_manager_ is null, so all MCP operations
        // become no-ops.
        if (cfg_.mcp_enabled) {
            body_->mcp = std::make_unique<mcp::McpServerManager>();
            // §Phase 15: install the per-server gate before load_config()
            // so disabled servers never enter configs_ in the first place.
            body_->mcp->set_disabled_servers(cfg_.mcp_servers_disabled);
            if (!cfg_.mcp_servers_path.empty()) {
                body_->mcp->load_config(cfg_.mcp_servers_path);
            }
            if (!cfg_.mcp_env_path.empty()) {
                body_->mcp->load_env_file(cfg_.mcp_env_path);  // §Phase 11: was load_env
            }
        }

        // §Phase 11: inject McpServerManager* into ToolFactory BEFORE
        // LLMClient::initialize() is called (which calls refresh_mcp_tools()).
        // This replaces all ::instance() call sites in ToolFactory.
        if (auto factory = ToolHandler::get_tool_factory()) {
            factory->set_mcp_manager(body_->mcp.get());
        }

        body_->client = std::make_unique<LLMClient>(
            app, body_->store, body_->mcp.get());
        if (sink_) {
            body_->client->set_sink(sink_);
        }
        body_->client->initialize();
    }

    // §Phase 6: register any host tools that haven't been registered with
    // the ToolFactory yet. We only register each tool once, so calling
    // initialize() multiple times (or after register_tool() before init)
    // doesn't double-register.
    if (auto factory = ToolHandler::get_tool_factory()) {
        for (auto& t : host_tools_) {
            if (factory->has_tool(t->get_name())) continue;
            factory->register_tool(
                std::make_shared<HostToolAdapter>(t, host_ctx_));
        }
    }

    // §Phase 9.7: Skill loading driven by AgentConfig. Previously the
    // library had a process-wide SkillManager singleton that registered
    // ~/.daedalus/skills/ in init_after_settings(), but AgentImpl never
    // called discover_all_skills(), so cached_skills_ stayed empty and
    // the system prompt's "Active Skills | 0" was always wrong.
    //
    // New contract: skills_dirs + skills on AgentConfig are the source
    // of truth. Both default to empty, so hosts that don't want skills
    // (embedded sandboxed agents, tests) get none — no surprise
    // injection from the singleton defaults. Hosts that do want them
    // (webui demo) set the fields explicitly.
    if (!cfg_.skills_dirs.empty() || !cfg_.skills.empty() ||
        !cfg_.skills_disabled.empty()) {
        try {
            auto* sm = SkillManager::get_instance();
            if (sm) {
                // §Phase 15: install the per-skill gate BEFORE discovery
                // so the catalog loop in build_system_instruction() never
                // sees disabled skills, and activate_skill() refuses them
                // at runtime.
                sm->set_disabled_skills(cfg_.skills_disabled);
                // Add developer-specified directories to the search
                // list (append, don't replace — the singleton's
                // defaults from init_after_settings() are still useful
                // for tests that call the API directly).
                for (const auto& dir : cfg_.skills_dirs) {
                    sm->add_skill_directory(dir);
                }
                // Discover everything currently visible. This populates
                // cached_skills_, so the discover_skills tool returns
                // the installed skills and the system prompt's Active
                // Skills count reflects reality.
                auto discovered = sm->discover_all_skills();
                // Activate any skills the host asked for by name. If a
                // name isn't in the cache (typo / not installed),
                // surface it via on_log instead of failing silently.
                size_t activated = 0;
                for (const auto& name : cfg_.skills) {
                    auto loaded = sm->load_skill_by_name(name);
                    if (loaded && sm->activate_skill(*loaded)) {
                        ++activated;
                    } else {
                        if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
                            "[SKILLS] Requested skill not found or not activatable: '"
                            + name + "' (not in any configured skills_dir)");
                    }
                }
                if (sink_) {
                    sink_->on_log(daedalus::LogLevel::Info,
                        "[SKILLS] Discovered " + std::to_string(discovered.size())
                        + " skill(s) from " + std::to_string(cfg_.skills_dirs.size())
                        + " dir(s); activated " + std::to_string(activated)
                        + " of " + std::to_string(cfg_.skills.size()) + " requested");
                }

                // §Phase 15: typo-detection for the disabled list.
                // Warn if a name in skills_disabled doesn't match any
                // discovered skill — likely a typo. Not an error since
                // skills may be installed later or via a different dir.
                if (!cfg_.skills_disabled.empty() && sink_) {
                    std::set<std::string> known;
                    for (const auto& s : sm->get_all_cached_skills()) {
                        known.insert(s.first);
                    }
                    for (const auto& name : cfg_.skills_disabled) {
                        if (!known.count(name)) {
                            sink_->on_log(daedalus::LogLevel::Warn,
                                "[SKILLS] skills_disabled entry '" + name
                                + "' does not match any discovered skill (typo?)");
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
                std::string("[SKILLS] Discovery failed: ") + e.what());
        }
    }

    // §Phase 15: typo-detection for the mcp_servers_disabled list.
    // Same idea as the skills one — warn if a disabled name doesn't
    // match any loaded server.
    //
    // §UB fix (2026-07-14): do NOT call list_configured_servers() twice
    // in the range-constructor expression. Each call returns a fresh
    // `vector<string>` by value, so `.begin()` and `.end()` end up
    // pointing into *different* containers — the resulting iterator
    // pair is ill-formed per [forward.iterators], and the set's range
    // constructor dereferences invalid memory. Symptom: SIGSEGV during
    // `_M_insert_unique_` (single-element inserts inside the unrolled
    // range loop). With creds resolved from the user's real config.json
    // this branch always runs; before, it was skipped because no
    // default model existed and `has_explicit_creds` was false.
    if (!cfg_.mcp_servers_disabled.empty() && body_->mcp && sink_) {
        const auto configured_servers = body_->mcp->list_configured_servers();
        std::set<std::string> known_servers(
            configured_servers.begin(), configured_servers.end());
        for (const auto& name : cfg_.mcp_servers_disabled) {
            if (!known_servers.count(name)) {
                sink_->on_log(daedalus::LogLevel::Warn,
                    "[MCP] mcp_servers_disabled entry '" + name
                    + "' does not match any loaded server (typo?)");
            }
        }
    }

    initialized_ = true;
    set_state(AgentState::Idle);
    if (sink_) {
        sink_->on_log(daedalus::LogLevel::Info,
                      "Agent::initialize() — ready"
                      + std::string(has_explicit_creds ? "" : " (no LLM client; no model configured)"));
    }
}

void AgentImpl::shutdown()
{
    if (!initialized_) return;
    // §Phase 11: disconnect MCP servers before destroying the client.
    // Safe to call on nullptr (no-op).
    if (body_->mcp) {
        body_->mcp->disconnect_all();
    }
    if (body_->client) {
        body_->client->cleanup();
        body_->client.reset();
    }
    body_->mcp.reset();   // §Phase 11
    initialized_ = false;
    set_state(AgentState::Done);
    if (sink_) {
        sink_->on_log(daedalus::LogLevel::Info,
                      "Agent::shutdown() — done");
    }
}

void AgentImpl::submit_user_message(std::string text)
{
    pending_user_messages_.push_back(std::move(text));
}

void AgentImpl::submit_user_message(std::string text,
                                    std::vector<ImageAttachment> /*images*/)
{
    // §Phase 1+: image attachment forwarding to Messages will be added in a
    // follow-on patch — the LLMClient's `run_one_turn(text)` doesn't accept
    // images yet at this layer.
    pending_user_messages_.push_back(std::move(text));
}

void AgentImpl::cancel() noexcept
{
    cancel_requested_.store(true, std::memory_order_release);
    if (body_->cancel_source.stop_possible()) {
        body_->cancel_source.request_stop();
    }
    // §Phase 5.1: also fire the LLMClient's cancel_source_ so the
    // SSE wait loop and per-iteration check exit promptly. Previously
    // the body_->cancel_source was a stranded signal — nothing in
    // the LLMClient read its token, so Agent::cancel() was effectively
    // a no-op for in-flight turns. Now the path is wired end-to-end.
    if (body_->client) {
        body_->client->cancel();
    }
}

void AgentImpl::run_one_turn(std::stop_token st)
{
    if (!initialized_) initialize();
    if (cancel_requested_.exchange(false)) {
        set_state(AgentState::Cancelled);
        return;
    }
    if (pending_user_messages_.empty()) {
        // No queued message — no-op.
        return;
    }
    if (st.stop_requested()) {
        set_state(AgentState::Cancelled);
        return;
    }

    if (!body_->client) {
        // No LLMClient spun up (no model credentials configured). Surface
        // as Error rather than crashing.
        std::string msg = std::move(pending_user_messages_.front());
        pending_user_messages_.erase(pending_user_messages_.begin());
        if (sink_) {
            sink_->on_log(daedalus::LogLevel::Error,
                          "Agent::run_one_turn() — no model configured; "
                          "cannot dispatch message: " + msg);
        }
        set_state(AgentState::Error);
        return;
    }

    std::string user_msg = std::move(pending_user_messages_.front());
    pending_user_messages_.erase(pending_user_messages_.begin());

    set_state(AgentState::Understanding);
    ::AgentState r = body_->client->run_one_turn(user_msg);
    set_state(map_from_internal(r));
}

AgentStats AgentImpl::stats() const noexcept
{
    AgentStats s = stats_;
    if (body_->client) {
        s.iteration         = body_->client->get_iteration_count();
        s.max_iterations    = body_->client->get_max_iterations();
        s.prompt_tokens     = body_->client->get_prompt_tokens();
        s.completion_tokens = body_->client->get_completion_tokens();
        s.total_tokens      = body_->client->get_total_tokens();
        s.max_context_size  = body_->client->get_max_context_size();
    }
    return s;
}

std::vector<ToolInfo> AgentImpl::list_tools() const
{
    // Surface ToolFactory::get_all_definitions() as ToolInfo records.
    std::vector<ToolInfo> out;
    auto factory = ToolHandler::get_tool_factory();
    if (!factory) return out;
    json defs = factory->get_all_definitions();
    if (!defs.is_array()) return out;
    out.reserve(defs.size());
    for (const auto& def : defs) {
        ToolInfo ti;
        if (def.contains("function")) {
            const auto& fn = def["function"];
            ti.name        = fn.value("name", "");
            ti.description = fn.value("description", "");
            if (fn.contains("parameters")) ti.schema = fn["parameters"];
        } else {
            ti.name        = def.value("name", "");
            ti.description = def.value("description", "");
            if (def.contains("parameters")) ti.schema = def["parameters"];
        }
        if (!ti.name.empty()) out.push_back(std::move(ti));
    }
    return out;
}

std::vector<SkillInfo> AgentImpl::list_skills() const
{
    // §Phase 1+: surface SkillManager's discovered skills. The Agent's
    // initialize() runs discovery; if it wasn't called yet, return empty.
    std::vector<SkillInfo> out;
    SkillManager* sm = SkillManager::get_instance();
    if (!sm) return out;
    auto skills = sm->discover_all_skills();
    out.reserve(skills.size());
    for (const auto& s : skills) {
        if (!s.is_valid()) continue;
        SkillInfo si;
        si.name        = s.metadata.name;
        si.description = s.metadata.description;
        si.source_path = s.path.string();
        out.push_back(std::move(si));
    }
    return out;
}

std::vector<ModelInfo> AgentImpl::list_models() const
{
    std::vector<ModelInfo> out;
    if (!body_->store) return out;
    const auto models = body_->store->list_models();
    out.reserve(models.size());
    for (const auto& cfg : models) {
        ModelInfo mi;
        mi.name            = cfg.name;
        mi.model_id        = cfg.model_id;
        mi.host            = cfg.host;
        mi.chat_endpoint   = cfg.chat_endpoint;
        mi.max_context_size = (cfg.max_context_size == 0)
                                ? body_->store->get_max_context_size_for_model(cfg.name)
                                : cfg.max_context_size;
        mi.temperature = cfg.temperature;  // §Phase 8.7
        out.push_back(std::move(mi));
    }
    return out;
}

ModelInfo AgentImpl::active_model() const noexcept
{
    // §Phase 8.5+: report the model that's actually loaded for this
    // session. Resolution priority (highest first):
    //   1. cfg.model_name (explicit --model argument)
    //   2. settings store default_model
    //   3. first model in settings store
    if (!body_->store) return {};
    const auto models = body_->store->list_models();

    auto find_by_name = [&](const std::string& name) -> std::optional<ModelInfo> {
        for (const auto& cfg : models) {
            if (cfg.name == name) {
                ModelInfo mi;
                mi.name = cfg.name;
                mi.model_id = cfg.model_id;
                mi.host = cfg.host;
                mi.chat_endpoint = cfg.chat_endpoint;
                mi.max_context_size = (cfg.max_context_size == 0)
                    ? body_->store->get_max_context_size_for_model(name)
                    : cfg.max_context_size;
                mi.temperature = cfg.temperature;  // §Phase 8.7
                return mi;
            }
        }
        return std::nullopt;
    };

    // 1. cfg.model_name
    if (!cfg_.model_name.empty()) {
        if (auto m = find_by_name(cfg_.model_name)) return *m;
    }
    // 2. default_model from settings store
    try {
        auto default_name = body_->store->get_default_model_name();
        if (!default_name.empty()) {
            if (auto m = find_by_name(default_name)) return *m;
        }
    } catch (...) { /* not fatal */ }
    // 3. first model in settings store
    if (!models.empty()) {
        const auto& cfg = models.front();
        ModelInfo mi;
        mi.name = cfg.name;
        mi.model_id = cfg.model_id;
        mi.host = cfg.host;
        mi.chat_endpoint = cfg.chat_endpoint;
        mi.max_context_size = (cfg.max_context_size == 0)
            ? body_->store->get_max_context_size_for_model(cfg.name)
            : cfg.max_context_size;
        mi.temperature = cfg.temperature;  // §Phase 8.7
        return mi;
    }
    return {};
}

std::string AgentImpl::library_version() noexcept
{
#ifdef DAEDALUS_GIT_VERSION
    return DAEDALUS_GIT_VERSION;
#else
    return "v0.0.0";
#endif
}

bool AgentImpl::auto_approve() const noexcept
{
    // Effective auto_approve = cfg.auto_approve OR settings store.
    // The library's LLMClient applies both — see LLMClient ctor where it
    // calls `if (settings_store->get_auto_approve()) ToolHandler::set_auto_mode(true)`.
    if (cfg_.auto_approve) return true;
    if (body_->store) {
        try {
            return body_->store->get_auto_approve();
        } catch (...) { /* fall through */ }
    }
    return false;
}

nlohmann::json AgentImpl::execute_tool(const std::string& name,
                                       const nlohmann::json& args)
{
    if (!initialized_) initialize();
    if (sink_) {
        sink_->on_log(daedalus::LogLevel::Warn,
                      "Agent::execute_tool() — Phase 1+ stub; "
                      "direct tool dispatch will be wired through the "
                      "host's tool path in Phase 7. (tool: " + name + ")");
    }
    (void)args;
    return nlohmann::json{{"status", "error"},
                          {"message", "Phase 1+ stub — see AgentImpl::execute_tool"}};
}

void AgentImpl::register_tool(std::unique_ptr<HostTool> tool)
{
    if (!tool) return;

    // 1. Take ownership in our own vector (so the tool survives).
    std::shared_ptr<HostTool> shared = std::move(tool);
    host_tools_.push_back(shared);

    // 2. Wrap in HostToolAdapter and register with the ToolFactory so
    //    the LLMClient's tool-dispatch path finds it.
    if (body_->client) {
        auto factory = ToolHandler::get_tool_factory();
        if (factory) {
            auto adapter = std::make_shared<HostToolAdapter>(shared, host_ctx_);
            factory->register_tool(adapter);
            if (sink_) {
                sink_->on_log(daedalus::LogLevel::Info,
                              "[TOOL] Registered host tool: " + shared->get_name());
            }
        }
    } else if (sink_) {
        sink_->on_log(daedalus::LogLevel::Warn,
                      "[TOOL] Agent not yet initialized; tool '"
                      + shared->get_name()
                      + "' will be registered on next initialize()");
    }
}

void AgentImpl::clear_memory()
{
    // §Phase 8.8: per-agent namespace (see initialize()'s same call).
    SessionMemory mem(resolve_agent_dir(cfg_.project_root, cfg_.agent_name));
    mem.clear_all_memories();
    if (sink_) {
        sink_->on_log(daedalus::LogLevel::Info,
                      "[MEMORY] All session memory cleared for this project");
    }
}

bool AgentImpl::switch_model(const std::string& model_name)
{
    if (!body_->store) return false;
    auto opt = body_->store->get_model(model_name);
    if (!opt.has_value()) return false;
    body_->store->set_default_model_name(model_name);
    cfg_.model_name = model_name;
    if (sink_) {
        sink_->on_log(daedalus::LogLevel::Info,
                      "[MODEL] Switched to " + model_name);
    }
    return true;
}

std::string AgentImpl::last_assistant_text() const
{
    if (!body_->client) return {};
    return body_->client->get_last_assistant_text();
}

}  // namespace daedalus::detail