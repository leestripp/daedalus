// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/A2aAgentBackend.cpp

#include "A2aAgentBackend.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

namespace daedalus_cli {

namespace a2a = daedalus::a2a;

// ============================================================================
// Streaming sink: delegates to the base sink and forwards assistant deltas
// to the A2A event handler as artifact updates.
// ============================================================================

class A2aAgentBackend::StreamingSink : public daedalus::AgentSink {
public:
    StreamingSink(daedalus::AgentSink& base,
                  a2a::IA2aEventHandler& handler,
                  std::string task_id,
                  std::string context_id)
        : base_(base)
        , handler_(handler)
        , task_id_(std::move(task_id))
        , context_id_(std::move(context_id)) {}

    void on_assistant_delta(std::string_view text) noexcept override {
        base_.on_assistant_delta(text);
        buffer_ += text;
        a2a::TaskArtifactUpdateEvent ev;
        ev.task_id = task_id_;
        ev.context_id = context_id_;
        ev.artifact.artifact_id = "answer";
        ev.artifact.name = "answer";
        ev.artifact.parts.push_back(a2a::Part::from_text(std::string(text), "text/plain"));
        ev.last_chunk = false;
        handler_.on_artifact_update(ev);
    }

    void on_tool_call(std::string_view name,
                      std::string_view call_id,
                      const nlohmann::json& args) noexcept override {
        base_.on_tool_call(name, call_id, args);
    }

    void on_tool_result(std::string_view name,
                        std::string_view call_id,
                        const nlohmann::json& result) noexcept override {
        base_.on_tool_result(name, call_id, result);
    }

    void on_tool_progress(std::string_view name,
                          std::string_view call_id,
                          const nlohmann::json& progress) noexcept override {
        base_.on_tool_progress(name, call_id, progress);
    }

    void on_state_change(int from, int to) noexcept override {
        base_.on_state_change(from, to);
    }

    void on_log(daedalus::LogLevel level, std::string_view message) noexcept override {
        base_.on_log(level, message);
    }

    void on_compaction(std::size_t before, std::size_t after) noexcept override {
        base_.on_compaction(before, after);
    }

    void on_bell() noexcept override { base_.on_bell(); }

    bool request_command_approval(std::string_view command) override {
        return base_.request_command_approval(command);
    }

    std::string buffer_;

private:
    daedalus::AgentSink& base_;
    a2a::IA2aEventHandler& handler_;
    std::string task_id_;
    std::string context_id_;
};

// ============================================================================
// Construction
// ============================================================================

A2aAgentBackend::A2aAgentBackend(daedalus::AgentConfig base_config,
                                 std::shared_ptr<daedalus::AgentSink> sink,
                                 ToolRegistrar registrar,
                                 std::string base_url,
                                 std::string name,
                                 std::string description)
    : base_config_(std::move(base_config))
    , sink_(std::move(sink))
    , registrar_(std::move(registrar))
    , base_url_(std::move(base_url))
    , name_(std::move(name))
    , description_(std::move(description)) {}

void A2aAgentBackend::set_base_url(std::string url) {
    base_url_ = std::move(url);
}

// ============================================================================
// Agent Card
// ============================================================================

a2a::AgentCard A2aAgentBackend::get_agent_card() const {
    a2a::AgentCard c;
    c.name = name_;
    c.description = description_;
    c.version = "1.0.0";
    a2a::AgentInterface iface;
    iface.url = base_url_ + "/v1";
    iface.transport = "HTTP+JSON";
    iface.protocol_version = "1.0";
    c.supported_interfaces.push_back(std::move(iface));
    c.capabilities.streaming = true;
    c.default_input_modes = {"text/plain"};
    c.default_output_modes = {"text/plain"};
    return c;
}

// ============================================================================
// Helpers
// ============================================================================

std::string A2aAgentBackend::extract_text(const std::vector<a2a::Part>& parts) const {
    std::string out;
    for (const auto& p : parts) {
        if (p.text) out += *p.text;
    }
    return out;
}

a2a::Task A2aAgentBackend::run_task(const a2a::Message& message,
                                    a2a::IA2aEventHandler* handler,
                                    a2a::A2aError& err) {
    auto n = task_counter_.fetch_add(1);
    std::string task_id = "task-" + std::to_string(n);
    std::string context_id = "ctx-" + std::to_string(n);

    std::string text = extract_text(message.parts);
    if (text.empty()) text = "(empty message)";

    auto cfg = base_config_;
    cfg.session_id = task_id;

    std::shared_ptr<daedalus::AgentSink> active_sink = sink_;
    std::shared_ptr<StreamingSink> stream_sink;
    if (handler) {
        stream_sink = std::make_shared<StreamingSink>(*sink_, *handler,
                                                      task_id, context_id);
        active_sink = stream_sink;
    }

    try {
        auto agent = std::make_unique<daedalus::Agent>(std::move(cfg), active_sink);
        registrar_(*agent);
        agent->initialize();

        if (handler) {
            a2a::TaskStatusUpdateEvent ev;
            ev.task_id = task_id;
            ev.context_id = context_id;
            ev.status.state = a2a::TaskState::working;
            handler->on_status_update(ev);
        }

        agent->submit_user_message(std::move(text));
        agent->run_one_turn();

        std::string answer;
        if (stream_sink) {
            answer = stream_sink->buffer_;
        } else {
            answer = agent->last_assistant_text();
        }
        if (answer.empty()) answer = "(no response)";

        a2a::Task task;
        task.id = task_id;
        task.context_id = context_id;
        task.status.state = a2a::TaskState::completed;
        a2a::Artifact art;
        art.artifact_id = "answer";
        art.name = "answer";
        art.parts.push_back(a2a::Part::from_text(std::move(answer), "text/plain"));
        task.artifacts.push_back(std::move(art));

        {
            std::lock_guard lock(tasks_mutex_);
            tasks_[task_id] = TaskRecord{task, std::move(agent)};
        }

        if (handler) {
            handler->on_task(task);
        }

        return task;
    } catch (const std::exception& e) {
        err = a2a::A2aError{500, "AGENT_ERROR", e.what(), nullptr};
        return {};
    }
}

// ============================================================================
// Backend methods
// ============================================================================

a2a::A2aResult<a2a::SendMessageResult> A2aAgentBackend::send_message(
    const a2a::SendMessageRequest& request) {
    a2a::A2aError err{0, "", "", nullptr};
    auto task = run_task(request.message, nullptr, err);
    if (err.http_status != 0) return std::unexpected(err);

    a2a::SendMessageResult result;
    result.task = std::move(task);
    return result;
}

a2a::A2aResult<a2a::Task> A2aAgentBackend::get_task(
    const a2a::GetTaskRequest& request) {
    std::lock_guard lock(tasks_mutex_);
    auto it = tasks_.find(request.id);
    if (it == tasks_.end()) {
        return std::unexpected(a2a::A2aError{404, "TASK_NOT_FOUND",
                                             "task not found: " + request.id, nullptr});
    }
    return it->second.task;
}

a2a::A2aResult<a2a::ListTasksResponse> A2aAgentBackend::list_tasks(
    const a2a::ListTasksRequest&) {
    a2a::ListTasksResponse r;
    std::lock_guard lock(tasks_mutex_);
    r.tasks.reserve(tasks_.size());
    for (const auto& [id, rec] : tasks_) {
        (void)id;
        r.tasks.push_back(rec.task);
    }
    r.total_size = static_cast<int>(r.tasks.size());
    r.page_size = r.total_size;
    return r;
}

a2a::A2aResult<a2a::Task> A2aAgentBackend::cancel_task(
    const a2a::CancelTaskRequest& request) {
    std::lock_guard lock(tasks_mutex_);
    auto it = tasks_.find(request.id);
    if (it == tasks_.end()) {
        return std::unexpected(a2a::A2aError{404, "TASK_NOT_FOUND",
                                             "task not found: " + request.id, nullptr});
    }
    if (it->second.agent) {
        it->second.agent->cancel();
    }
    it->second.task.status.state = a2a::TaskState::canceled;
    return it->second.task;
}

a2a::A2aResult<void> A2aAgentBackend::send_streaming_message(
    const a2a::SendMessageRequest& request,
    a2a::IA2aEventHandler& handler) {
    a2a::A2aError err{0, "", "", nullptr};
    auto task = run_task(request.message, &handler, err);
    if (err.http_status != 0) return std::unexpected(err);
    (void)task;
    return {};
}

a2a::A2aResult<void> A2aAgentBackend::subscribe_to_task(
    const a2a::SubscribeToTaskRequest& request,
    a2a::IA2aEventHandler& handler) {
    std::lock_guard lock(tasks_mutex_);
    auto it = tasks_.find(request.id);
    if (it == tasks_.end()) {
        return std::unexpected(a2a::A2aError{404, "TASK_NOT_FOUND",
                                             "task not found: " + request.id, nullptr});
    }
    handler.on_task(it->second.task);
    return {};
}

} // namespace daedalus_cli
