/**
 * @file src/library/Agent.cpp
 * @brief Implementation of the daedalus::Agent facade.
 *
 * Holds std::unique_ptr<detail::AgentImpl>. The PIMPL ensures the public
 * Agent.h header stays small and doesn't drag in the heavy subsystem
 * types (LLMClient, McpServerManager, ...).
 */
#include "daedalus/Agent.h"

#include "AgentImpl.h"

#include <stop_token>
#include <utility>

namespace daedalus {

Agent::Agent(AgentConfig cfg, std::shared_ptr<AgentSink> sink)
    : impl_(std::make_unique<detail::AgentImpl>(std::move(cfg), std::move(sink)))
{}

Agent::~Agent() = default;
Agent::Agent(Agent&&) noexcept = default;
Agent& Agent::operator=(Agent&&) noexcept = default;

// ---- Lifecycle -------------------------------------------------------------

void Agent::initialize()       { impl_->initialize(); }
void Agent::shutdown()         { impl_->shutdown(); }
// §CRASH-4 (2026-07-16): forward the crash-handler install/uninstall
// to AgentImpl, which forwards to LLMClient's Messages instance.
void Agent::install_crash_handlers()   { impl_->install_crash_handlers(); }
void Agent::uninstall_crash_handlers() { impl_->uninstall_crash_handlers(); }

// ---- Conversation control --------------------------------------------------

void Agent::run_one_turn(std::stop_token st)                 { impl_->run_one_turn(st); }
void Agent::submit_user_message(std::string text)             { impl_->submit_user_message(std::move(text)); }
void Agent::submit_user_message(std::string text,
                                std::vector<ImageAttachment> imgs)
{
    impl_->submit_user_message(std::move(text), std::move(imgs));
}
void Agent::cancel() noexcept { impl_->cancel(); }

// ---- Read-only views -------------------------------------------------------

AgentState  Agent::state() const noexcept { return impl_->state(); }
AgentStats  Agent::stats() const noexcept { return impl_->stats(); }
const AgentConfig& Agent::config() const noexcept { return impl_->config(); }

std::string_view Agent::agent_name() const noexcept { return impl_->agent_name(); }

std::vector<ToolInfo>  Agent::list_tools()  const { return impl_->list_tools(); }
std::vector<SkillInfo> Agent::list_skills() const { return impl_->list_skills(); }
std::vector<ModelInfo> Agent::list_models() const { return impl_->list_models(); }

std::vector<std::string> Agent::mcp_configured_servers() const
{
    // §FIX-MCP-STORE-LOAD (2026-07-17): thin forwarder. Previously
    // unreachable from outside the library — AgentImpl built an
    // McpServerManager but never loaded it from the store, so this
    // would have returned []. Now returns whatever was loaded by
    // AgentImpl::AgentImpl's load_from_store() + load_config() chain.
    return impl_->mcp_configured_servers();
}

ModelInfo Agent::active_model() const noexcept { return impl_->active_model(); }
bool        Agent::auto_approve() const noexcept { return impl_->auto_approve(); }
std::string Agent::library_version() noexcept   { return detail::AgentImpl::library_version(); }

ModelDiscoveryResult Agent::discover_models(
    const std::string& model_name,
    const std::filesystem::path& daedalus_home)
{
    return detail::AgentImpl::discover_models(model_name, daedalus_home);
}

nlohmann::json Agent::execute_tool(const std::string& name,
                                   const nlohmann::json& args)
{
    return impl_->execute_tool(name, args);
}

void Agent::register_tool(std::unique_ptr<BaseTool> tool)
{
    impl_->register_tool(std::move(tool));
}

void Agent::clear_memory() { impl_->clear_memory(); }

bool Agent::switch_model(const std::string& model_name)
{
    return impl_->switch_model(model_name);
}

std::string Agent::last_assistant_text() const
{
    return impl_->last_assistant_text();
}

}  // namespace daedalus