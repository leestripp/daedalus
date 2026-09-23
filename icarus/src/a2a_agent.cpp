// SPDX-License-Identifier: BSL-1.0
// src/a2a_agent.cpp
//
// A2A Agent implementation — icarus as an A2A (Agent2Agent Protocol) agent.
//
// Runs an HTTP server using daedalus::a2a::A2aServer and a custom
// IA2aTaskBackend that drives a daedalus::Agent per task.

#include "a2a_agent.h"

#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include <csignal>
#include <chrono>

#include <daedalus/paths.h>
#include <daedalus/types/AgentState.h>
#include <daedalus/types/ErrorKind.h>

#include "util.h"

namespace a2a {

namespace {

// Global pointer used by the signal handler to stop the server.
std::atomic<daedalus::a2a::A2aServer*> g_server{nullptr};

void signal_handler(int /*sig*/) {
    if (auto* s = g_server.load(std::memory_order_acquire)) {
        s->stop();
    }
}

// Generate a simple task id.
std::string make_task_id() {
    static std::atomic<int> counter{0};
    return "task_" + std::to_string(++counter);
}

// Generate a simple message id.
std::string make_message_id() {
    static std::atomic<int> counter{0};
    return "msg_" + std::to_string(++counter);
}

// ============================================================================
// Sink that converts daedalus events into collected output (and optional
// streaming A2A events).
// ============================================================================
class A2aAgentSink : public daedalus::AgentSink {
public:
    explicit A2aAgentSink(daedalus::a2a::IA2aEventHandler* handler = nullptr)
        : handler_(handler) {}

    void on_assistant_delta(std::string_view text) noexcept override {
        std::lock_guard<std::mutex> g(mu_);
        assistant_text_ += std::string(text);
    }

    bool request_command_approval(std::string_view /*command*/) override {
        // A2A agent daemon auto-approves bash commands. A future
        // improvement could surface this to the A2A client via a status
        // update and block for a response.
        return true;
    }

    void on_tool_call(std::string_view /*name*/,
                      std::string_view /*call_id*/,
                      const nlohmann::json& /*args*/) noexcept override {
        // No-op for now; could emit status updates for long-running tools.
    }

    void on_tool_result(std::string_view /*name*/,
                        std::string_view /*call_id*/,
                        const nlohmann::json& /*result*/) noexcept override {
        // No-op for now.
    }

    // §on_error: structured error channel. Terminal LLM failures still end
    // the turn with AgentState::Done, so the backend can only detect them
    // here — record the last terminal error for run_task() to check.
    void on_error(daedalus::ErrorKind kind,
                  std::string_view message,
                  bool will_retry) noexcept override {
        if (will_retry) return;  // transient — the library is retrying
        std::lock_guard<std::mutex> g(mu_);
        terminal_error_ = "[" + std::string(daedalus::to_string(kind)) + "] "
                          + std::string(message);
    }

    void on_state_change(int /*from*/, int to) noexcept override {
        if (!handler_) return;
        daedalus::a2a::TaskStatusUpdateEvent ev;
        ev.task_id = task_id_;
        // Map the real daedalus::AgentState enum onto A2A task states.
        // (Raw-int mapping used to be wrong: it predates the Documenting
        // state and sent Acting→completed, Verifying→failed, etc.)
        using daedalus::AgentState;
        switch (static_cast<AgentState>(to)) {
            case AgentState::Idle:
                ev.status.state = daedalus::a2a::TaskState::submitted; break;
            case AgentState::Understanding:
            case AgentState::Planning:
            case AgentState::Acting:
            case AgentState::Verifying:
            case AgentState::Documenting:
            case AgentState::Compressing:
                ev.status.state = daedalus::a2a::TaskState::working; break;
            case AgentState::AwaitingApproval:
                ev.status.state = daedalus::a2a::TaskState::input_required; break;
            case AgentState::Done:
                ev.status.state = daedalus::a2a::TaskState::completed; break;
            case AgentState::Cancelled:
                ev.status.state = daedalus::a2a::TaskState::canceled; break;
            case AgentState::Error:
                ev.status.state = daedalus::a2a::TaskState::failed; break;
            default:
                ev.status.state = daedalus::a2a::TaskState::unspecified; break;
        }
        handler_->on_status_update(ev);
    }

    void set_task_id(std::string id) { task_id_ = std::move(id); }

    std::string assistant_text() const {
        std::lock_guard<std::mutex> g(mu_);
        return assistant_text_;
    }

    // Last terminal (will_retry=false) LLM error, if any.
    std::optional<std::string> terminal_error() const {
        std::lock_guard<std::mutex> g(mu_);
        return terminal_error_;
    }

private:
    daedalus::a2a::IA2aEventHandler* handler_;
    mutable std::mutex mu_;
    std::string assistant_text_;
    std::string task_id_;
    std::optional<std::string> terminal_error_;
};

// ============================================================================
// Task store and backend.
// ============================================================================
struct StoredTask {
    daedalus::a2a::Task task;
    std::shared_ptr<daedalus::Agent> agent;
};

class IcarusA2aBackend : public daedalus::a2a::IA2aTaskBackend {
public:
    IcarusA2aBackend(std::filesystem::path project_root,
                     daedalus::AgentConfig cfg_template)
        : project_root_(std::move(project_root))
        , cfg_template_(std::move(cfg_template)) {}

    // The URL advertised in the agent card. Set once the server has bound
    // so the card reflects the real address/port instead of a guess.
    void set_advertised_url(std::string url) {
        std::lock_guard<std::mutex> g(mu_);
        advertised_url_ = std::move(url);
    }

    [[nodiscard]] daedalus::a2a::AgentCard get_agent_card() const override {
        daedalus::a2a::AgentCard card;
        card.name = "icarus";
        card.description = "Autonomous coding agent powered by daedalus";
        card.version = daedalus::Agent::library_version();
        card.capabilities.streaming = true;
        card.capabilities.push_notifications = false;
        card.capabilities.extended_agent_card = false;
        card.default_input_modes = {"text"};
        card.default_output_modes = {"text"};

        daedalus::a2a::AgentInterface iface;
        {
            std::lock_guard<std::mutex> g(mu_);
            iface.url = advertised_url_.empty()
                ? "http://localhost:8000"
                : advertised_url_;
        }
        iface.transport = "http";
        iface.protocol_version = "0.1";
        card.supported_interfaces.push_back(std::move(iface));
        return card;
    }

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::SendMessageResult> send_message(
        const daedalus::a2a::SendMessageRequest& request) override {
        auto task = run_task(request.message, nullptr);
        if (!task) {
            return std::unexpected(daedalus::a2a::A2aError{
                0, "TASK_FAILED", "Failed to process message", nullptr});
        }
        daedalus::a2a::SendMessageResult result;
        result.task = std::move(*task);
        return result;
    }

    [[nodiscard]] daedalus::a2a::A2aResult<void> send_streaming_message(
        const daedalus::a2a::SendMessageRequest& request,
        daedalus::a2a::IA2aEventHandler& handler) override {
        auto task = run_task(request.message, &handler);
        if (!task) {
            return std::unexpected(daedalus::a2a::A2aError{
                0, "TASK_FAILED", "Failed to process message", nullptr});
        }
        handler.on_complete();
        return {};
    }

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::Task> get_task(
        const daedalus::a2a::GetTaskRequest& request) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = tasks_.find(request.id);
        if (it == tasks_.end()) {
            return std::unexpected(daedalus::a2a::A2aError{
                404, "TASK_NOT_FOUND", "Task not found: " + request.id, nullptr});
        }
        return it->second.task;
    }

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::ListTasksResponse> list_tasks(
        const daedalus::a2a::ListTasksRequest& request) override {
        std::lock_guard<std::mutex> g(mu_);
        daedalus::a2a::ListTasksResponse resp;
        for (const auto& [id, stored] : tasks_) {
            if (request.context_id && stored.task.context_id != *request.context_id) {
                continue;
            }
            if (request.status && stored.task.status.state != *request.status) {
                continue;
            }
            resp.tasks.push_back(stored.task);
        }
        resp.total_size = static_cast<int>(resp.tasks.size());
        resp.page_size = resp.total_size;
        return resp;
    }

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::Task> cancel_task(
        const daedalus::a2a::CancelTaskRequest& request) override {
        std::lock_guard<std::mutex> g(mu_);
        auto it = tasks_.find(request.id);
        if (it == tasks_.end()) {
            return std::unexpected(daedalus::a2a::A2aError{
                404, "TASK_NOT_FOUND", "Task not found: " + request.id, nullptr});
        }
        if (it->second.agent) {
            it->second.agent->cancel();
        }
        it->second.task.status.state = daedalus::a2a::TaskState::canceled;
        return it->second.task;
    }

    [[nodiscard]] daedalus::a2a::A2aResult<void> subscribe_to_task(
        const daedalus::a2a::SubscribeToTaskRequest& /*request*/,
        daedalus::a2a::IA2aEventHandler& /*handler*/) override {
        // Not implemented for the first pass; A2A clients can poll get_task
        // or use send_streaming_message for new messages.
        return std::unexpected(daedalus::a2a::A2aError{
            501, "NOT_IMPLEMENTED", "subscribe_to_task is not implemented", nullptr});
    }

private:
    std::optional<daedalus::a2a::Task> run_task(
        const daedalus::a2a::Message& message,
        daedalus::a2a::IA2aEventHandler* handler) {
        // Build a single text prompt from text parts.
        std::ostringstream oss;
        for (const auto& part : message.parts) {
            if (part.kind == daedalus::a2a::PartKind::text && part.text) {
                oss << *part.text;
            }
        }
        std::string prompt = oss.str();
        if (prompt.empty()) {
            return std::nullopt;
        }

        // Start from the CLI-derived template (--model, --home, --skills,
        // --system-prompt, --no-memory, --agent-name) and force the
        // daemon-specific fields: per-task project root and auto-approve
        // (no interactive terminal exists in daemon mode).
        daedalus::AgentConfig cfg = cfg_template_;
        cfg.project_root = project_root_;
        cfg.max_agent_iterations = 600;
        cfg.auto_approve = true;

        auto sink = std::make_shared<A2aAgentSink>(handler);
        auto agent = std::make_shared<daedalus::Agent>(std::move(cfg), sink);

        try {
            agent->initialize();
        } catch (const std::exception& e) {
            std::cerr << "[a2a] Failed to initialize agent: " << e.what() << "\n";
            return std::nullopt;
        }

        std::string task_id = make_task_id();
        sink->set_task_id(task_id);

        daedalus::a2a::Task task;
        task.id = task_id;
        if (message.task_id) task.context_id = *message.task_id;
        task.status.state = daedalus::a2a::TaskState::submitted;

        {
            std::lock_guard<std::mutex> g(mu_);
            tasks_[task_id] = StoredTask{task, agent};
        }

        if (handler) {
            daedalus::a2a::TaskStatusUpdateEvent submitted;
            submitted.task_id = task_id;
            submitted.status.state = daedalus::a2a::TaskState::submitted;
            handler->on_status_update(submitted);
        }

        try {
            agent->submit_user_message(prompt);
            task.status.state = daedalus::a2a::TaskState::working;
            if (handler) {
                daedalus::a2a::TaskStatusUpdateEvent working;
                working.task_id = task_id;
                working.status.state = daedalus::a2a::TaskState::working;
                handler->on_status_update(working);
            }
            agent->run_one_turn();
        } catch (const std::exception& e) {
            std::cerr << "[a2a] Turn failed: " << e.what() << "\n";
            task.status.state = daedalus::a2a::TaskState::failed;
            {
                std::lock_guard<std::mutex> g(mu_);
                tasks_[task_id].task = task;
            }
            if (handler) {
                daedalus::a2a::TaskStatusUpdateEvent failed;
                failed.task_id = task_id;
                failed.status.state = daedalus::a2a::TaskState::failed;
                handler->on_status_update(failed);
                handler->on_task(task);
            }
            return task;
        }

        // A terminal LLM failure (auth, exhausted retries, ...) does not
        // throw — daedalus ends the turn with AgentState::Done and reports
        // the failure only through AgentSink::on_error. Fail the task
        // instead of reporting a misleading "completed".
        if (auto err = sink->terminal_error()) {
            task.status.state = daedalus::a2a::TaskState::failed;
            daedalus::a2a::Message err_msg;
            err_msg.message_id = make_message_id();
            err_msg.role = daedalus::a2a::Role::agent;
            err_msg.parts.push_back(daedalus::a2a::Part::from_text(*err));
            task.status.message = std::move(err_msg);
            {
                std::lock_guard<std::mutex> g(mu_);
                tasks_[task_id].task = task;
            }
            if (handler) {
                daedalus::a2a::TaskStatusUpdateEvent failed;
                failed.task_id = task_id;
                failed.status = task.status;
                handler->on_status_update(failed);
                handler->on_task(task);
            }
            return task;
        }

        task.status.state = daedalus::a2a::TaskState::completed;
        daedalus::a2a::Message reply;
        reply.message_id = make_message_id();
        reply.role = daedalus::a2a::Role::agent;
        reply.parts.push_back(daedalus::a2a::Part::from_text(sink->assistant_text()));
        task.history.push_back(message);
        task.history.push_back(reply);

        {
            std::lock_guard<std::mutex> g(mu_);
            tasks_[task_id].task = task;
            tasks_[task_id].agent.reset();
        }

        if (handler) {
            daedalus::a2a::TaskStatusUpdateEvent completed;
            completed.task_id = task_id;
            completed.status.state = daedalus::a2a::TaskState::completed;
            handler->on_status_update(completed);
            handler->on_task(task);
        }

        return task;
    }

    std::filesystem::path project_root_;
    daedalus::AgentConfig cfg_template_;
    std::string advertised_url_;
    mutable std::mutex mu_;
    std::unordered_map<std::string, StoredTask> tasks_;
};

} // anonymous namespace

// ============================================================================
// Public entry point
// ============================================================================

int run_a2a_agent(const CliArgs& args) {
    std::cerr << "[a2a] icarus A2A agent mode\n" << std::flush;

    std::filesystem::path cwd;
    try {
        cwd = std::filesystem::current_path();
    } catch (...) {
        cwd = ".";
    }

    // Build the per-task AgentConfig template from the CLI args, mirroring
    // what main() does for the interactive mode so --model/--home/--skills/
    // --system-prompt/--no-memory behave the same in daemon mode.
    daedalus::AgentConfig cfg_template;
    cfg_template.model_name     = args.model_override;
    cfg_template.memory_enabled = !args.no_memory;
    cfg_template.agent_name     = args.agent_name;
    cfg_template.daedalus_home  = args.daedalus_home.empty()
        ? daedalus::Paths::global().user_data_root()
        : args.daedalus_home;
    cfg_template.skills_dirs    = args.skills_dirs.empty()
        ? std::vector<std::filesystem::path>{daedalus::Paths::global().global_skills_dir()}
        : args.skills_dirs;
    cfg_template.system_prompt  = resolve_system_prompt(args.system_prompt_file, cwd);
    // §ContextCache (2026-07-31): stable per-project cache key fallback for
    // per-task agents (server-side prompt cache, e.g. Moonshot
    // prompt_cache_key), mirroring what main() does in interactive mode.
    {
        std::ostringstream sess;
        sess << "icarus-a2a-" << std::hex
             << std::hash<std::string>{}(std::filesystem::absolute(cwd).string());
        cfg_template.session_id = sess.str();
    }

    auto backend = std::make_shared<IcarusA2aBackend>(cwd, std::move(cfg_template));
    daedalus::a2a::A2aServerConfig cfg;
    cfg.address = "127.0.0.1";
    cfg.port = args.a2a_port;
    cfg.backend = backend;

    daedalus::a2a::A2aServer server(std::move(cfg));
    g_server.store(&server, std::memory_order_release);

    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::atomic<bool> start_failed{false};
    std::thread server_thread([&server, &start_failed]() {
        try {
            server.start();
        } catch (const std::exception& e) {
            std::cerr << "[a2a] Server error: " << e.what() << "\n";
            start_failed.store(true, std::memory_order_release);
        }
    });

    // Wait briefly for the server to bind so we can report the real URL.
    for (int i = 0; i < 50 && server.port() == 0 &&
         !start_failed.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (start_failed.load(std::memory_order_acquire) || server.port() == 0) {
        std::cerr << "[a2a] Failed to start server\n" << std::flush;
        g_server.store(nullptr, std::memory_order_release);
        if (server_thread.joinable()) server_thread.join();
        return 1;
    }

    // Advertise the real bound URL in the agent card.
    backend->set_advertised_url(server.base_url());
    std::cerr << "[a2a] Server ready at " << server.base_url() << "\n" << std::flush;

    server_thread.join();

    g_server.store(nullptr, std::memory_order_release);
    return 0;
}

} // namespace a2a
