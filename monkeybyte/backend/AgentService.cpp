// SPDX-License-Identifier: BSL-1.0
#include "AgentService.h"

#include <daedalus/paths.h>
#include <daedalus/settings.h>
#include <daedalus/types/BaseTool.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace monkeybyte {
namespace {

using nlohmann::json;

std::string utf8_prefix(std::string_view text, std::size_t maximum) {
    if (text.size() <= maximum) return std::string(text);
    std::size_t end = maximum;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U) --end;
    return std::string(text.substr(0, end)) + "…";
}

json bounded_json(const json& value) {
    const auto text = value.dump();
    if (text.size() <= 20000) return value;
    return json{{"truncated", true}, {"text", utf8_prefix(text, 20000)}};
}

nlohmann::json chimpflow_from_store(daedalus::SettingsStore& store) {
    nlohmann::json config = nlohmann::json::parse(
        R"json({"enabled":false,"readAssistant":true,"dictationEnabled":true,"serverUrl":"http://127.0.0.1","port":8090,"apiKey":"","ttsModel":"kokoro-82m","sttModel":"whisper","voice":""})json");
    try { config.update(nlohmann::json::parse(store.get_string("monkeybyte.chimpflow", "{}"))); }
    catch (const nlohmann::json::exception&) {}

    // GTK's canonical keys win when present. Electron originally used only
    // the JSON blob, which made the two frontends disagree about the same
    // local service and API key. Writes below update both representations.
    constexpr std::string_view missing = "\x1emonkeybyte-missing\x1e";
    const auto text = [&](std::string_view gtk, std::string_view electron) {
        const auto value = store.get_string(gtk, std::string(missing));
        if (value != missing) config[std::string(electron)] = value;
    };
    const auto boolean = [&](std::string_view gtk, std::string_view electron) {
        const auto value = store.get_string(gtk, std::string(missing));
        if (value != missing) config[std::string(electron)] = value == "1";
    };
    boolean("chimpflow.enabled", "enabled");
    boolean("chimpflow.read_assistant", "readAssistant");
    boolean("chimpflow.dictation_enabled", "dictationEnabled");
    text("chimpflow.server_url", "serverUrl");
    text("chimpflow.api_key", "apiKey");
    text("chimpflow.tts_model", "ttsModel");
    text("chimpflow.stt_model", "sttModel");
    text("chimpflow.voice", "voice");
    if (const auto value = store.get_string("chimpflow.port", std::string(missing)); value != missing) {
        try { config["port"] = std::stoi(value); } catch (const std::exception&) {}
    }
    return config;
}

class UiTool final : public daedalus::BaseTool {
public:
    explicit UiTool(UiHandler handler) : handler_(std::move(handler)) {}

    std::string get_name() const override { return "app_ui_control"; }
    std::string get_description() const override {
        return "Control the MonkeyByte application UI: move panel sizers, switch sidebar panels, "
               "set panel widths, open files in the editor, open Markdown/HTML previews of workspace "
               "files, open a URL (e.g. a local dev server you started) in a browser tab, navigate it, "
               "read the live JavaScript-rendered page text, run JavaScript (browser.eval — DOM state, "
               "clicks, form fills), list links, read captured console/page errors, upload local files, "
               "list/close browser tabs, read the current editor text selection, and read window geometry.";
    }
    json get_definition() const override {
        return {{"type", "function"}, {"function", {
            {"name", get_name()}, {"description", get_description()},
            {"parameters", {{"type", "object"}, {"properties", {
                {"command", {{"type", "string"}, {"description",
                    "sizer.begin | sizer.delta | sizer.end | panel.width | panel.switch | "
                    "editor.open | editor.open_as | browser.open | browser.navigate | browser.read | "
                    "browser.eval | browser.links | browser.console | browser.upload | browser.list | "
                    "browser.close | editor.selection | app.geometry | app.commands"}}},
                {"args", {{"type", "object"}, {"description", "Command arguments, including tab, path, "
                    "line, mode, url, selector, max_chars, script, max_entries, clear, dx, or width."}}}
            }}, {"required", json::array({"command"})}}}
        }}};
    }
    json execute(const json& args, daedalus::HostToolContext&) override {
        try {
            const auto command = args.at("command").get<std::string>();
            auto result = handler_(command, args.value("args", json::object()));
            if (!result.is_object()) result = {{"result", result}};
            result["status"] = result.contains("error") ? "error" : "ok";
            return result;
        } catch (const std::exception& error) {
            return {{"status", "error"}, {"error", error.what()}};
        }
    }
private:
    UiHandler handler_;
};

}  // namespace

struct AgentService::Approval {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<bool> decision;
};

class AgentService::Sink final : public daedalus::AgentSink {
public:
    explicit Sink(AgentService& service) : service_(service) {}
    void on_assistant_delta(std::string_view text) noexcept override {
        service_.emit("assistant_delta", {{"text", std::string(text)}});
    }
    void on_tool_call(std::string_view name, std::string_view id, const json& args) noexcept override {
        service_.emit("tool_call", {{"name", std::string(name)}, {"callId", std::string(id)},
                                    {"args", bounded_json(args)}});
    }
    void on_tool_result(std::string_view name, std::string_view id, const json& result) noexcept override {
        if (name == "tell_user") {
            service_.emit("tell_user", {{"message", result.value("content", result.value("message", result.dump()))}});
        }
        service_.emit("tool_result", {{"name", std::string(name)}, {"callId", std::string(id)},
                                      {"result", bounded_json(result)}});
        service_.emit_stats();
    }
    void on_tool_progress(std::string_view name, std::string_view id, const json& progress) noexcept override {
        service_.emit("tool_progress", {{"name", std::string(name)}, {"callId", std::string(id)},
                                        {"progress", bounded_json(progress)}});
    }
    void on_state_change(daedalus::AgentState, daedalus::AgentState to) noexcept override {
        service_.state_.store(static_cast<int>(to));
        service_.emit("status", service_.status());
        service_.emit_stats();
    }
    void on_error(daedalus::ErrorKind, std::string_view message, bool will_retry) noexcept override {
        service_.emit("error", {{"message", std::string(message)}, {"willRetry", will_retry}});
    }
    void on_log(daedalus::LogLevel level, std::string_view message) noexcept override {
        service_.emit("log", {{"level", static_cast<int>(level)}, {"message", utf8_prefix(message, 4000)}});
    }
    void on_turn_requested(std::string_view source) noexcept override {
        service_.emit("turn_requested", {{"source", std::string(source)}});
        service_.request_turn();
    }
    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        service_.emit("compaction", {{"beforeTokens", before}, {"afterTokens", after}});
        service_.emit_stats();
    }
    bool request_command_approval(std::string_view command) override {
        return service_.await_approval(command);
    }
    void on_command_auto_approved(std::string_view command, daedalus::ApprovalSource source) noexcept override {
        service_.emit("auto_approved", {{"command", std::string(command)},
                                        {"source", static_cast<int>(source)}});
    }
private:
    AgentService& service_;
};

AgentService::AgentService(std::filesystem::path index_path, EventCallback events, UiHandler ui)
    : daedalus_home_([] { const char* value = std::getenv("DAEDALUS_HOME"); return value ? std::filesystem::path(value) : std::filesystem::path(); }()),
      catalog_(std::move(index_path)), events_(std::move(events)), ui_(std::move(ui)),
      sink_(std::make_shared<Sink>(*this)) {
    driver_ = std::jthread([this](std::stop_token token) { run_driver(token); });
}

AgentService::~AgentService() {
    cancel();
    {
        std::lock_guard lock(driver_mutex_);
        driver_stop_ = true;
    }
    driver_cv_.notify_all();
    driver_.request_stop();
    if (driver_.joinable()) driver_.join();
    std::lock_guard lock(mutex_);
    if (agent_) agent_->shutdown();
}

void AgentService::emit(std::string type, json payload) const {
    try {
        events_(std::move(type), std::move(payload));
    } catch (const std::exception& error) {
        std::cerr << "event delivery failed: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "event delivery failed with an unknown error\n";
    }
}

void AgentService::require_idle() const {
    if (running_.load()) throw std::runtime_error("Stop the active turn before changing sessions or models.");
}

void AgentService::activate(const SessionRecord& record) {
    require_idle();
    std::lock_guard lock(mutex_);
    if (agent_) {
        agent_->shutdown();
        agent_.reset();
    }
    std::filesystem::create_directories(record.workspace);
    std::filesystem::current_path(record.workspace);
    daedalus::AgentConfig config;
    config.project_root = record.workspace;
    config.daedalus_home = daedalus_home_;
    config.agent_name = "monkeybyte";
    config.session_id = record.id;
    config.model_name = record.model;
    config.thinking_level = daedalus::thinking_level_from_string(record.thinking);
    config.stream = true;
    config.user_agent = "MonkeyByte-Electron/0.1";
    config.skills_dirs = {daedalus::Paths::global().global_skills_dir()};
    auto store = daedalus_home_.empty() ? daedalus::SettingsStore::instance()
        : daedalus::SettingsStore::for_home(daedalus_home_);
    config.max_agent_iterations = store.get_max_agent_iterations();
    config.max_tool_result_size = store.get_max_tool_result_size();
    config.max_request_size = store.get_max_request_size();
    config.max_response_size = store.get_max_response_size();
    config.mcp_enabled = store.get_bool("agent.mcp_enabled", true);
    try {
        const auto disabled_tools = json::parse(store.get_string("monkeybyte.disabled_tools", "[]"));
        const auto disabled_skills = json::parse(store.get_string("monkeybyte.disabled_skills", "[]"));
        config.tools_disabled = disabled_tools.get<std::set<std::string>>();
        config.skills_disabled = disabled_skills.get<std::set<std::string>>();
        const auto prompts = json::parse(store.get_string("monkeybyte.system_prompts", "[]"));
        const auto bindings = json::parse(store.get_string("monkeybyte.model_prompts", "{}"));
        const auto model = record.model.empty() ? store.get_default_model_name() : record.model;
        std::string prompt_name = bindings.value(model, "");
        if (!prompt_name.empty()) {
            const auto found = std::ranges::find_if(prompts, [&](const auto& prompt) { return prompt.value("name", "") == prompt_name; });
            if (found != prompts.end()) config.system_prompt = found->value("text", "");
        } else {
            const auto workspace_prompts = json::parse(store.get_string("monkeybyte.workspace_prompts", "{}"));
            config.system_prompt = workspace_prompts.value(record.workspace.string(), "");
            if (config.system_prompt.empty()) {
                const auto found = std::ranges::find_if(prompts, [](const auto& prompt) { return prompt.value("isDefault", false); });
                if (found != prompts.end()) config.system_prompt = found->value("text", "");
            }
        }
    } catch (const json::exception&) {
        // A malformed host-only setting must not prevent the agent from opening.
    }
    for (const auto& server : store.list_mcp_servers()) {
        if (!server.enabled) config.mcp_servers_disabled.insert(server.name);
    }
    auto agent = std::make_unique<daedalus::Agent>(config, sink_);
    agent->initialize();
    agent->set_remember_approvals(false);
    agent->register_tool(std::make_unique<UiTool>(ui_));
    record_ = record;
    agent_ = std::move(agent);
    state_.store(static_cast<int>(agent_->state()));
    refresh_history();
    emit("session", record_->to_json());
    emit("history", history());
    emit("status", status());
    emit_stats();
}

nlohmann::json AgentService::ensure_session(const std::filesystem::path& workspace,
                                            std::string_view requested_id) {
    if (workspace.empty() || !std::filesystem::is_directory(workspace)) {
        throw std::invalid_argument("Select an existing workspace directory.");
    }
    require_idle();
    const auto record = catalog_.ensure(workspace, requested_id);
    if (!record_ || record_->id != record.id) activate(record);
    return {{"session", record.to_json()}, {"sessions", sessions()}, {"status", status()},
            {"history", history()}};
}

nlohmann::json AgentService::create_session(const std::filesystem::path& workspace, std::string name) {
    require_idle();
    const auto record = catalog_.create(workspace, std::move(name));
    activate(record);
    return {{"session", record.to_json()}, {"sessions", sessions()}, {"history", history()}};
}

nlohmann::json AgentService::select_session(std::string_view id) {
    require_idle();
    const auto record = catalog_.select(id);
    activate(record);
    return {{"session", record.to_json()}, {"sessions", sessions()}, {"history", history()}};
}

nlohmann::json AgentService::rename_session(std::string_view id, std::string name) {
    const auto renamed = catalog_.rename(id, std::move(name));
    {
        std::lock_guard lock(mutex_);
        if (record_ && record_->id == renamed.id) record_ = renamed;
    }
    emit("sessions", sessions());
    return {{"session", renamed.to_json()}, {"sessions", sessions()}};
}

nlohmann::json AgentService::delete_session(std::string_view id) {
    require_idle();
    const auto current = catalog_.active();
    const auto removed = catalog_.select(id);
    activate(removed);
    {
        std::lock_guard lock(mutex_);
        agent_->clear_history();
    }
    const auto result = catalog_.remove(id);
    (void)result;
    const auto next = catalog_.active();
    if (next) activate(*next);
    else {
        std::lock_guard lock(mutex_);
        if (agent_) agent_->shutdown();
        agent_.reset();
        record_.reset();
        history_cache_ = json::array();
    }
    if (current && current->id != id && next && current->id != next->id) {
        activate(catalog_.select(current->id));
    }
    emit("sessions", sessions());
    return {{"deletedId", std::string(id)}, {"sessions", sessions()}, {"history", history()}};
}

nlohmann::json AgentService::send(std::string text, const json& attachments, std::string source) {
    return send_impl(std::move(text), attachments, std::move(source), false);
}

nlohmann::json AgentService::steer(std::string text, const json& attachments) {
    return send_impl(std::move(text), attachments, "steer:lee", true);
}

nlohmann::json AgentService::send_impl(std::string text, const json& attachments,
                                       std::string source, bool steering) {
    if (text.size() > 1000000) throw std::invalid_argument("Message exceeds the 1 MB limit.");
    if (text.empty() && attachments.empty()) throw std::invalid_argument("Message is empty.");
    std::vector<daedalus::ImageAttachment> images;
    for (const auto& item : attachments) {
        const auto filename = item.is_string() ? item.get<std::string>() : item.at("path").get<std::string>();
        if (!std::filesystem::is_regular_file(filename)) throw std::invalid_argument("Attachment does not exist: " + filename);
        images.push_back({.source = daedalus::ImageAttachment::Source::Path, .path_or_url = filename});
    }
    {
        std::lock_guard lock(mutex_);
        if (!agent_) throw std::runtime_error("No active session.");
        if (steering) agent_->submit_steer_message(text, images, std::move(source));
        else agent_->submit_user_message(text, images, std::move(source));
    }
    emit("user_message", {{"text", std::move(text)}, {"attachments", attachments}});
    request_turn();
    return status();
}

void AgentService::request_turn() {
    {
        std::lock_guard lock(driver_mutex_);
        turn_requested_ = true;
    }
    driver_cv_.notify_all();
}

void AgentService::run_driver(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        {
            std::unique_lock lock(driver_mutex_);
            driver_cv_.wait(lock, [&] { return driver_stop_ || turn_requested_; });
            if (driver_stop_) return;
            turn_requested_ = false;
        }
        {
            std::lock_guard agent_lock(mutex_);
            if (!agent_ || agent_->pending_count() == 0) continue;
        }
        running_.store(true);
        emit("status", status());
        try {
            while (!stop_token.stop_requested()) {
                std::size_t pending = 0;
                {
                    std::lock_guard agent_lock(mutex_);
                    if (!agent_) break;
                    pending = agent_->pending_count();
                }
                if (pending == 0) break;
                // submit_user_message(), cancel(), pending_count(), and cancel_source_
                // remain available while this call runs. activate() is gated by running_.
                agent_->run_one_turn();
            }
            if (agent_->turn_had_terminal_error()) {
                const auto error = agent_->last_error();
                emit("turn_error", {{"message", error ? error->message : "The turn failed."}});
            }
        } catch (const std::exception& error) {
            emit("turn_error", {{"message", error.what()}});
        } catch (...) {
            emit("turn_error", {{"message", "Unknown agent turn failure."}});
        }
        {
            std::lock_guard agent_lock(mutex_);
            refresh_history();
            running_.store(false);
            state_.store(static_cast<int>(agent_->state()));
        }
        emit("history", history());
        emit_stats();
        emit("status", status());
        emit("turn_done", {{"cancelled", agent_->state() == daedalus::AgentState::Cancelled}});
        reject_approvals();
    }
}

nlohmann::json AgentService::cancel() {
    reject_approvals();
    std::lock_guard lock(mutex_);
    if (agent_) agent_->cancel();
    return status();
}

nlohmann::json AgentService::respond(std::string_view id, bool approved) {
    std::lock_guard lock(approval_mutex_);
    const auto found = approvals_.find(std::string(id));
    if (found == approvals_.end()) throw std::invalid_argument("Approval is no longer pending.");
    {
        std::lock_guard approval_lock(found->second->mutex);
        found->second->decision = approved;
    }
    found->second->cv.notify_all();
    return {{"ok", true}};
}

void AgentService::reject_approvals() {
    std::lock_guard lock(approval_mutex_);
    for (auto& [id, approval] : approvals_) {
        std::lock_guard approval_lock(approval->mutex);
        approval->decision = false;
        approval->cv.notify_all();
    }
}

bool AgentService::await_approval(std::string_view command) {
    const auto id = std::to_string(next_approval_++);
    auto approval = std::make_shared<Approval>();
    {
        std::lock_guard lock(approval_mutex_);
        approvals_[id] = approval;
    }
    emit("approval", {{"id", id}, {"command", std::string(command)}});
    std::unique_lock lock(approval->mutex);
    approval->cv.wait(lock, [&] { return approval->decision.has_value(); });
    const auto decision = *approval->decision;
    lock.unlock();
    {
        std::lock_guard approval_lock(approval_mutex_);
        approvals_.erase(id);
    }
    emit("approval_resolved", {{"id", id}, {"approved", decision}});
    return decision;
}

void AgentService::refresh_history() {
    if (!agent_) {
        history_cache_ = json::array();
        return;
    }
    history_cache_ = json::array();
    for (const auto& entry : agent_->history_snapshot()) {
        if (entry.role == "system" || entry.content.empty()) continue;
        history_cache_.push_back({{"role", entry.role}, {"content", entry.content},
                                  {"source", entry.source}, {"receivedAt", entry.received_at}});
    }
}

nlohmann::json AgentService::history() const {
    std::lock_guard lock(mutex_);
    return history_cache_;
}

nlohmann::json AgentService::clear_history() {
    require_idle();
    std::lock_guard lock(mutex_);
    if (!agent_) throw std::runtime_error("No active session.");
    agent_->clear_history();
    refresh_history();
    emit("history", history_cache_);
    emit_stats();
    return history_cache_;
}

nlohmann::json AgentService::status() const {
    std::lock_guard lock(mutex_);
    std::size_t pending = 0;
    if (agent_) pending = agent_->pending_count();
    return {{"busy", running_.load()}, {"pending", pending},
            {"state", daedalus::to_string(static_cast<daedalus::AgentState>(state_.load()))},
            {"sessionId", record_ ? json(record_->id) : json(nullptr)},
            {"workspace", record_ ? json(record_->workspace.string()) : json(nullptr)},
            {"model", record_ ? json(record_->model) : json(nullptr)},
            {"thinking", record_ ? json(record_->thinking) : json(nullptr)},
            {"stats", stats()}};
}

nlohmann::json AgentService::stats() const {
    std::lock_guard lock(mutex_);
    if (!agent_) return json::object();
    const auto value = agent_->stats();
    return {{"iteration", value.iteration}, {"maxIterations", value.max_iterations},
        {"promptTokens", value.prompt_tokens}, {"completionTokens", value.completion_tokens},
        {"totalTokens", value.total_tokens}, {"cachedTokens", value.cached_tokens},
        {"maxContextSize", value.max_context_size}, {"messageCount", value.message_count},
        {"pendingToolCalls", value.pending_tool_calls}, {"executingToolCalls", value.executing_tool_calls},
        {"completedToolCalls", value.completed_tool_calls}, {"failedToolCalls", value.failed_tool_calls}};
}

void AgentService::emit_stats() const {
    emit("stats", stats());
}

nlohmann::json AgentService::settings() const {
    auto store = daedalus_home_.empty() ? daedalus::SettingsStore::instance()
        : daedalus::SettingsStore::for_home(daedalus_home_);
    json providers = json::array();
    for (const auto& provider : store.list_providers_detailed()) {
        providers.push_back({{"name", provider.name}, {"host", provider.host},
            {"chatEndpoint", provider.chat_endpoint}, {"discoveryEndpoint", provider.discovery_endpoint},
            {"timeoutSeconds", provider.timeout_seconds}, {"plugin", provider.plugin},
            {"hasApiKey", !provider.api_key.empty()}});
    }
    json models = json::array();
    for (const auto& model : store.list_models()) {
        models.push_back({{"name", model.name}, {"provider", model.provider},
            {"modelId", model.model_id}, {"maxContextSize", model.max_context_size},
            {"temperature", model.temperature}});
    }
    json servers = json::array();
    for (const auto& server : store.list_mcp_servers()) {
        json env = json::array();
        for (const auto& [key, value] : server.env) env.push_back(key);
        servers.push_back({{"name", server.name}, {"transport", server.transport},
            {"command", server.command}, {"args", server.args}, {"envKeys", env},
            {"url", server.url}, {"description", server.description}, {"enabled", server.enabled},
            {"hasBearerToken", !server.bearer_token.empty()}});
    }
    json search = json::array();
    for (const auto& provider : store.get_web_search_providers()) {
        search.push_back({{"name", provider.name}, {"enabled", provider.enabled},
            {"hasApiKey", !provider.api_key.empty()}, {"baseUrl", provider.base_url},
            {"username", provider.username}, {"hasPassword", !provider.password.empty()},
            {"priority", provider.priority}});
    }
    auto stored_json = [&](std::string_view key, const char* fallback) {
        try { return json::parse(store.get_string(key, fallback)); }
        catch (const json::exception&) { return json::parse(fallback); }
    };
    auto chimpflow = chimpflow_from_store(store);
    const char* environment_key = std::getenv("CHIMPFLOW_KEY");
    chimpflow["hasApiKey"] = !chimpflow.value("apiKey", "").empty()
        || (environment_key && *environment_key);
    chimpflow.erase("apiKey");
    return {{"providers", providers}, {"models", models},
        {"defaultModel", store.get_default_model_name()}, {"autoApprove", store.get_auto_approve()},
        {"agent", {{"maxIterations", store.get_max_agent_iterations()},
            {"maxToolResultSize", store.get_max_tool_result_size()},
            {"maxRequestSize", store.get_max_request_size()},
            {"maxResponseSize", store.get_max_response_size()},
            {"mcpEnabled", store.get_bool("agent.mcp_enabled", true)},
            {"disabledTools", stored_json("monkeybyte.disabled_tools", "[]")},
            {"disabledSkills", stored_json("monkeybyte.disabled_skills", "[]")}}},
        {"mcpServers", servers}, {"webSearch", search},
        {"prompts", stored_json("monkeybyte.system_prompts", "[]")},
        {"modelPrompts", stored_json("monkeybyte.model_prompts", "{}")},
        {"chimpflow", chimpflow},
        {"paths", {{"home", daedalus_home_.string()},
            {"globalSkills", daedalus::Paths::global().global_skills_dir().string()}}}};
}

nlohmann::json AgentService::chimpflow_config() const {
    auto store = daedalus_home_.empty() ? daedalus::SettingsStore::instance()
        : daedalus::SettingsStore::for_home(daedalus_home_);
    return chimpflow_from_store(store);
}

nlohmann::json AgentService::select_model(std::string model) {
    require_idle();
    {
        std::lock_guard lock(mutex_);
        if (!agent_ || !record_) throw std::runtime_error("No active session.");
        if (!agent_->switch_model(model)) throw std::invalid_argument("Unknown model: " + model);
        record_->model = model;
        refresh_history();
    }
    const auto record = catalog_.set_model(record_->id, std::move(model));
    emit("history", history());
    emit("session", record.to_json());
    return {{"session", record.to_json()}, {"history", history()}, {"settings", settings()}};
}

nlohmann::json AgentService::select_thinking(std::string level) {
    require_idle();
    const auto parsed = daedalus::thinking_level_from_string(level);
    {
        std::lock_guard lock(mutex_);
        if (!agent_ || !record_) throw std::runtime_error("No active session.");
        agent_->set_thinking_level(parsed);
        record_->thinking = level;
    }
    const auto record = catalog_.set_thinking(record_->id, level);
    return {{"session", record.to_json()}, {"status", status()}};
}

nlohmann::json AgentService::tools() const {
    std::lock_guard lock(mutex_);
    if (!agent_) throw std::runtime_error("No active session.");
    auto result = json::array();
    for (const auto& tool : agent_->list_tools()) result.push_back(tool.name);
    return result;
}

nlohmann::json AgentService::execute_ui(std::string command, nlohmann::json args) {
    return ui_(std::move(command), std::move(args));
}

nlohmann::json AgentService::update_settings(const json& patch) {
    require_idle();
    auto store = daedalus_home_.empty() ? daedalus::SettingsStore::instance()
        : daedalus::SettingsStore::for_home(daedalus_home_);
    bool rebuild = false;
    if (patch.contains("defaultModel")) { store.set_default_model_name(patch.at("defaultModel").get<std::string>()); rebuild = true; }
    if (patch.contains("autoApprove")) {
        const auto enabled = patch.at("autoApprove").get<bool>();
        store.set_auto_approve(enabled);
        std::lock_guard lock(mutex_);
        if (agent_) agent_->set_auto_approve(enabled);
    }
    if (patch.contains("provider")) {
        const auto& value = patch.at("provider");
        daedalus::ProviderConfig provider;
        provider.name = value.at("name").get<std::string>();
        const auto existing = store.get_provider(provider.name);
        provider.api_key = value.value("apiKey", existing ? existing->api_key : "");
        provider.host = value.value("host", existing ? existing->host : "");
        provider.chat_endpoint = value.value("chatEndpoint", existing ? existing->chat_endpoint : "/v1/chat/completions");
        provider.discovery_endpoint = value.value("discoveryEndpoint", existing ? existing->discovery_endpoint : "/v1/models");
        provider.timeout_seconds = value.value("timeoutSeconds", existing ? existing->timeout_seconds : 30);
        provider.plugin = value.value("plugin", existing ? existing->plugin : "");
        store.upsert_provider(provider);
        rebuild = true;
    }
    if (patch.contains("model")) {
        const auto& value = patch.at("model");
        daedalus::ModelConfig model;
        model.name = value.at("name").get<std::string>();
        model.provider = value.at("provider").get<std::string>();
        model.model_id = value.at("modelId").get<std::string>();
        const auto existing = store.get_model(model.name);
        model.max_context_size = value.value("maxContextSize", existing ? existing->max_context_size : 0);
        model.temperature = value.value("temperature", existing ? existing->temperature : 0.0);
        store.upsert_model(model);
        rebuild = true;
    }
    if (patch.contains("agent")) {
        const auto& agent = patch.at("agent");
        if (agent.contains("maxIterations")) store.set_max_agent_iterations(agent.at("maxIterations").get<int>());
        if (agent.contains("maxToolResultSize")) store.set_max_tool_result_size(agent.at("maxToolResultSize").get<std::size_t>());
        if (agent.contains("maxRequestSize")) store.set_max_request_size(agent.at("maxRequestSize").get<std::size_t>());
        if (agent.contains("maxResponseSize")) store.set_max_response_size(agent.at("maxResponseSize").get<std::size_t>());
        if (agent.contains("mcpEnabled")) store.set_bool("agent.mcp_enabled", agent.at("mcpEnabled").get<bool>());
        if (agent.contains("disabledTools")) store.set_string("monkeybyte.disabled_tools", agent.at("disabledTools").dump());
        if (agent.contains("disabledSkills")) store.set_string("monkeybyte.disabled_skills", agent.at("disabledSkills").dump());
        rebuild = true;
    }
    if (patch.contains("mcpServer")) {
        const auto& value = patch.at("mcpServer");
        daedalus::SettingsStore::McpServer server;
        server.name = value.at("name").get<std::string>();
        const auto existing = store.get_mcp_server(server.name);
        server.transport = value.value("transport", existing ? existing->transport : "stdio");
        server.command = value.value("command", existing ? existing->command : "");
        server.args = value.value("args", existing ? existing->args : std::vector<std::string>{});
        server.env = existing ? existing->env : std::map<std::string, std::string>{};
        if (value.contains("env")) {
            for (const auto& [key, item] : value.at("env").items()) {
                if (!item.get<std::string>().empty()) server.env[key] = item.get<std::string>();
            }
        }
        server.url = value.value("url", existing ? existing->url : "");
        server.bearer_token = value.value("bearerToken", "");
        if (server.bearer_token.empty() && existing) server.bearer_token = existing->bearer_token;
        server.description = value.value("description", existing ? existing->description : "");
        server.enabled = value.value("enabled", existing ? existing->enabled : true);
        store.upsert_mcp_server(server); rebuild = true;
    }
    if (patch.contains("webSearchProvider")) {
        const auto& value = patch.at("webSearchProvider");
        daedalus::WebSearchProviderSettings provider;
        provider.name = value.at("name").get<std::string>();
        const auto all = store.get_web_search_providers();
        const auto found = std::ranges::find(all, provider.name, &daedalus::WebSearchProviderSettings::name);
        provider.enabled = value.value("enabled", found != all.end() ? found->enabled : false);
        provider.api_key = value.value("apiKey", "");
        if (provider.api_key.empty() && found != all.end()) provider.api_key = found->api_key;
        provider.base_url = value.value("baseUrl", found != all.end() ? found->base_url : "");
        provider.username = value.value("username", found != all.end() ? found->username : "");
        provider.password = value.value("password", "");
        if (provider.password.empty() && found != all.end()) provider.password = found->password;
        provider.priority = value.value("priority", found != all.end() ? found->priority : 0);
        store.set_web_search_provider(provider);
    }
    auto prompts = json::parse(store.get_string("monkeybyte.system_prompts", "[]"));
    if (patch.contains("prompt")) {
        const auto& value = patch.at("prompt");
        const auto name = value.at("name").get<std::string>();
        prompts.erase(std::remove_if(prompts.begin(), prompts.end(), [&](const auto& item) { return item.value("name", "") == name; }), prompts.end());
        if (value.value("isDefault", false)) for (auto& item : prompts) item["isDefault"] = false;
        prompts.push_back({{"name", name}, {"text", value.value("text", "")}, {"isDefault", value.value("isDefault", false)}});
        store.set_string("monkeybyte.system_prompts", prompts.dump()); rebuild = true;
    }
    if (patch.contains("deletePrompt")) {
        const auto name = patch.at("deletePrompt").get<std::string>();
        prompts.erase(std::remove_if(prompts.begin(), prompts.end(), [&](const auto& item) { return item.value("name", "") == name; }), prompts.end());
        store.set_string("monkeybyte.system_prompts", prompts.dump()); rebuild = true;
    }
    if (patch.contains("modelPrompt")) {
        const auto& value = patch.at("modelPrompt");
        auto bindings = json::parse(store.get_string("monkeybyte.model_prompts", "{}"));
        const auto model = value.at("model").get<std::string>();
        const auto prompt = value.value("prompt", "");
        if (prompt.empty()) bindings.erase(model); else bindings[model] = prompt;
        store.set_string("monkeybyte.model_prompts", bindings.dump()); rebuild = true;
    }
    if (patch.contains("workspacePrompt")) {
        const auto& value = patch.at("workspacePrompt");
        auto prompts_by_workspace = json::parse(store.get_string("monkeybyte.workspace_prompts", "{}"));
        prompts_by_workspace[value.at("workspace").get<std::string>()] = value.value("prompt", "");
        store.set_string("monkeybyte.workspace_prompts", prompts_by_workspace.dump()); rebuild = true;
    }
    if (patch.contains("chimpflow")) {
        auto value = chimpflow_from_store(store);
        value.update(patch.at("chimpflow"));
        if (patch.at("chimpflow").value("apiKey", "").empty())
            value["apiKey"] = chimpflow_from_store(store).value("apiKey", "");
        store.set_string("monkeybyte.chimpflow", value.dump());
        store.set_string("chimpflow.enabled", value.value("enabled", false) ? "1" : "0");
        store.set_string("chimpflow.read_assistant", value.value("readAssistant", true) ? "1" : "0");
        store.set_string("chimpflow.dictation_enabled", value.value("dictationEnabled", true) ? "1" : "0");
        store.set_string("chimpflow.server_url", value.value("serverUrl", "http://127.0.0.1"));
        store.set_string("chimpflow.port", std::to_string(value.value("port", 8090)));
        store.set_string("chimpflow.api_key", value.value("apiKey", ""));
        store.set_string("chimpflow.tts_model", value.value("ttsModel", "kokoro-82m"));
        store.set_string("chimpflow.stt_model", value.value("sttModel", "whisper"));
        store.set_string("chimpflow.voice", value.value("voice", ""));
    }
    if (patch.contains("deleteProvider")) { store.delete_provider(patch.at("deleteProvider").get<std::string>()); rebuild = true; }
    if (patch.contains("deleteModel")) { store.delete_model(patch.at("deleteModel").get<std::string>()); rebuild = true; }
    if (patch.contains("deleteMcpServer")) { store.delete_mcp_server(patch.at("deleteMcpServer").get<std::string>()); rebuild = true; }
    if (rebuild && record_) activate(*record_);
    emit("settings", settings());
    return settings();
}

}  // namespace monkeybyte
