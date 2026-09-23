// SPDX-License-Identifier: BSL-1.0
// src/a2a_tools.h
//
// A2A tool — exposes daedalus::a2a::A2aClient as a daedalus BaseTool.
//
// Allows the daedalus agent to interact with remote A2A (Agent2Agent)
// agents using the A2A protocol built into the daedalus library.

#pragma once

#include <string>
#include <memory>

#include <daedalus/a2a/A2aClient.h>
#include <daedalus/types/BaseTool.h>

namespace a2a {

/**
 * @brief A2A tool for daedalus agents.
 *
 * Exposes the A2A client as a daedalus BaseTool with actions:
 *   get_agent_card, send_message, send_streaming_message,
 *   get_task, list_tasks, cancel_task, subscribe_to_task
 */
class A2aTool : public daedalus::BaseTool {
public:
    /**
     * @param default_url A2A server URL used when no `a2a_url` param
     *                     is passed in the tool call.
     */
    explicit A2aTool(std::string default_url = "http://localhost:8000");

    [[nodiscard]] std::string get_name() const override;
    [[nodiscard]] std::string get_description() const override;
    [[nodiscard]] nlohmann::json get_definition() const override;

    nlohmann::json execute(const nlohmann::json& args,
                          daedalus::HostToolContext& ctx) override;

private:
    std::string default_url_;
};

} // namespace a2a
