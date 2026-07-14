/**
 * @file src/tools/HostToolAdapter.h
 * @brief Adapter that wraps a public daedalus::HostTool as an internal
 *        BaseTool, so host-registered tools flow through the same
 *        ToolFactory dispatch path as built-in tools.
 *
 * §Phase 6: hosts call daedalus::Agent::register_tool(unique_ptr<HostTool>);
 * AgentImpl wraps it in a HostToolAdapter and hands the adapter to the
 * ToolFactory. The factory's normal lookup → execute_tool() path then
 * dispatches to the HostTool's execute() with a HostToolContext.
 */
#pragma once

#include <memory>
#include <string>

#include "BaseTool.h"
#include "daedalus/AgentSink.h"   // for HostToolContext
#include "daedalus/types/HostTool.h"
#include "daedalus/types/HostToolContext.h"

using json = nlohmann::json;

/**
 * @brief Wraps a daedalus::HostTool as a BaseTool.
 *
 * The adapter holds:
 *   - the wrapped HostTool (kept alive via shared_ptr)
 *   - a HostToolContext that the AgentImpl provides (it knows the
 *     project's FileIO / project root).
 *
 * execute(args, file_io) is delegated to host_tool->execute(args, *ctx).
 * The file_io parameter is ignored — HostToolContext is the public API.
 */
class HostToolAdapter : public BaseTool {
public:
    HostToolAdapter(std::shared_ptr<daedalus::HostTool> tool,
                    std::shared_ptr<daedalus::HostToolContext> ctx)
        : tool_(std::move(tool)), ctx_(std::move(ctx)) {}

    [[nodiscard]] std::string get_name() const override { return tool_->get_name(); }
    [[nodiscard]] std::string get_description() const override { return tool_->get_description(); }
    [[nodiscard]] json        get_definition() const override { return tool_->get_definition(); }

    json execute(const json& args, FileIO& /*file_io*/) override {
        return tool_->execute(args, *ctx_);
    }

private:
    std::shared_ptr<daedalus::HostTool>          tool_;
    std::shared_ptr<daedalus::HostToolContext>   ctx_;
};