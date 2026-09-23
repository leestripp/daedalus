// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-cli/A2aAgentBackend.h
//
// A2A task backend that drives a daedalus::Agent for each incoming task.
// Used by the CLI demo's --a2a-server mode.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <daedalus.h>
#include <daedalus/a2a/A2aServer.h>

namespace daedalus_cli {

class A2aAgentBackend : public daedalus::a2a::IA2aTaskBackend {
public:
    using ToolRegistrar = std::function<void(daedalus::Agent&)>;

    A2aAgentBackend(daedalus::AgentConfig base_config,
                    std::shared_ptr<daedalus::AgentSink> sink,
                    ToolRegistrar registrar,
                    std::string base_url,
                    std::string name = "daedalus-cli",
                    std::string description = "daedalus-cli A2A server");

    void set_base_url(std::string url);

    [[nodiscard]] daedalus::a2a::AgentCard get_agent_card() const override;

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::SendMessageResult> send_message(
        const daedalus::a2a::SendMessageRequest& request) override;

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::Task> get_task(
        const daedalus::a2a::GetTaskRequest& request) override;

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::ListTasksResponse> list_tasks(
        const daedalus::a2a::ListTasksRequest& request) override;

    [[nodiscard]] daedalus::a2a::A2aResult<daedalus::a2a::Task> cancel_task(
        const daedalus::a2a::CancelTaskRequest& request) override;

    [[nodiscard]] daedalus::a2a::A2aResult<void> send_streaming_message(
        const daedalus::a2a::SendMessageRequest& request,
        daedalus::a2a::IA2aEventHandler& handler) override;

    [[nodiscard]] daedalus::a2a::A2aResult<void> subscribe_to_task(
        const daedalus::a2a::SubscribeToTaskRequest& request,
        daedalus::a2a::IA2aEventHandler& handler) override;

private:
    struct TaskRecord {
        daedalus::a2a::Task task;
        std::shared_ptr<daedalus::Agent> agent;
    };

    class StreamingSink;

    [[nodiscard]] daedalus::a2a::Task run_task(
        const daedalus::a2a::Message& message,
        daedalus::a2a::IA2aEventHandler* handler,
        daedalus::a2a::A2aError& err);

    [[nodiscard]] std::string extract_text(
        const std::vector<daedalus::a2a::Part>& parts) const;

    daedalus::AgentConfig base_config_;
    std::shared_ptr<daedalus::AgentSink> sink_;
    ToolRegistrar registrar_;
    std::string base_url_;
    std::string name_;
    std::string description_;

    std::mutex tasks_mutex_;
    std::unordered_map<std::string, TaskRecord> tasks_;
    std::atomic<uint64_t> task_counter_{0};
};

} // namespace daedalus_cli
