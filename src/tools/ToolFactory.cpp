#include "tools/ToolFactory.h"
#include "tools/FileTool.h"
#include "tools/McpProxiedTool.h"
#include "tools/SearchTool.h"
#include "tools/UserTool.h"
#include "tools/SetStatusTool.h"
#include "tools/GitTool.h"
#include "tools/BashTool.h"
#include "tools/PDFTool.h"
#include "tools/SkillTool.h"
#include "tools/MemoryTool.h"
#include "tools/SystemInfoTool.h"
#include "tools/BackgroundTool.h"
#include "tools/NetworkTool.h"
#include "mcp/McpServerManager.h"
#include "core/FileIO.h"
#include "core/ThreadPool.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <future>
#include <thread>
#include <vector>
ToolFactory::ToolFactory() {
    initialize_tools();
}

// §Phase 11: injected McpServerManager* (nullptr = MCP disabled).
void ToolFactory::set_mcp_manager(mcp::McpServerManager* mcp_manager) {
    mcp_manager_ = mcp_manager;
}

void ToolFactory::register_tool(std::shared_ptr<BaseTool> tool) {
    if (tool) {
        m_tools.push_back(tool);
    }
}
void ToolFactory::initialize_tools() {
    // File operation tools
    register_tool(std::make_shared<ReadFileTool>());
    register_tool(std::make_shared<ListDirectoryTool>());
    register_tool(std::make_shared<CreateFileTool>());
    register_tool(std::make_shared<CreateDirectoryTool>());
    register_tool(std::make_shared<ModifyFileTool>());
    register_tool(std::make_shared<EditFilePatchTool>());
    register_tool(std::make_shared<DeleteFileTool>());
    register_tool(std::make_shared<DeleteDirectoryTool>());
    // PDF tools
    register_tool(std::make_shared<ReadPDFTool>());
    // MCP tools are NOT registered statically. They are populated dynamically
    // by `refresh_mcp_tools()` (called from LLMClient) so each server's
    // `tools/list` becomes one `McpProxiedTool` named `mcp__<server>__<tool>`.
    // See Agent.md §18.
    // Search tools
    register_tool(std::make_shared<GrepSearchTool>());
    register_tool(std::make_shared<ReadFileLinesTool>());
    register_tool(std::make_shared<ReadFileHeadTool>());
    register_tool(std::make_shared<ReadFileTailTool>());
    register_tool(std::make_shared<ListDirectoryTreeTool>());
    register_tool(std::make_shared<WikiSearchTool>());
    register_tool(std::make_shared<SerperSearchTool>());
    // User interaction tools
    register_tool(std::make_shared<TellUserTool>());
    register_tool(std::make_shared<AskUserTool>());
    register_tool(std::make_shared<CompressContextTool>());
    register_tool(std::make_shared<StopTool>());
    // §State Machine: agent-driven state transitions.
    register_tool(std::make_shared<SetStatusTool>());
    // Git tools
    register_tool(std::make_shared<GitStatusTool>());
    register_tool(std::make_shared<GitBranchTool>());
    register_tool(std::make_shared<GitCommitTool>());
    register_tool(std::make_shared<GitAddTool>());
    register_tool(std::make_shared<GitSwitchTool>());
    register_tool(std::make_shared<GitDiffTool>());
    register_tool(std::make_shared<GitPushTool>());
    register_tool(std::make_shared<GitMergeTool>());
    // Bash tool
    register_tool(std::make_shared<BashTool>());
    // Skill tools
    register_tool(std::make_shared<DiscoverSkillsTool>());
    register_tool(std::make_shared<LoadSkillTool>());
    register_tool(std::make_shared<GetActiveSkillsTool>());
    register_tool(std::make_shared<SearchSkillsTool>());
    register_tool(std::make_shared<DeactivateSkillTool>());
    register_tool(std::make_shared<GetSkillContextTool>());
    // Memory tools
    register_tool(std::make_shared<RememberTool>());
    register_tool(std::make_shared<ForgetTool>());
    register_tool(std::make_shared<MemoriesTool>());
    register_tool(std::make_shared<MemoryStatsTool>());
    // System tools
    register_tool(std::make_shared<SystemInfoTool>());
    // Background process tool
    register_tool(std::make_shared<BackgroundTool>());
    // Network tool - HTTP/HTTPS via OmniNet (replaces Bash curl)
    register_tool(std::make_shared<HttpRequestTool>());
    // Network diagnostic tools - DNS / TCP / TLS
    register_tool(std::make_shared<DnsLookupTool>());
    register_tool(std::make_shared<TcpCheckTool>());
    register_tool(std::make_shared<TlsInspectTool>());
    // WebSocket tools - long-lived connections, shared registry
    register_tool(std::make_shared<WebSocketConnectTool>());
    register_tool(std::make_shared<WebSocketSendTool>());
    register_tool(std::make_shared<WebSocketRecvTool>());
    register_tool(std::make_shared<WebSocketCloseTool>());
    // Navigation tool
    // (removed NaviTool - no longer needed)
}
json ToolFactory::get_all_definitions() const {
    json definitions = json::array();
    for (const auto& tool : m_tools) {
        definitions.push_back(tool->get_definition());
    }
    return definitions;
}
json ToolFactory::execute_tool(const std::string& tool_name, const json& args, FileIO& file_io) {
    for (const auto& tool : m_tools) {
        if (tool->get_name() == tool_name) {
            return tool->execute(args, file_io);
        }
    }
    return json::object({{"status", "error"}, {"message", "Unknown tool: " + tool_name}});
}

void ToolFactory::refresh_mcp_tools() {
    // 1) Drop every previously-registered McpProxiedTool.
    m_tools.erase(
        std::remove_if(m_tools.begin(), m_tools.end(),
            [](const std::shared_ptr<BaseTool>& t) {
                return std::dynamic_pointer_cast<McpProxiedTool>(t) != nullptr;
            }),
        m_tools.end());

    // §Phase 11: guard with mcp_manager_ (nullptr when mcp_enabled=false).
    // Previously called ::instance() — now uses the injected pointer.
    if (!mcp_manager_) return;
    auto& mgr = *mcp_manager_;
    if (!mgr.is_config_loaded()) {
        return;
    }

    // Snapshot the configured-servers list because the manager's internal
    // connection state may change concurrently with this walk (HTTP
    // servers, list_changed handler).
    const auto configured = mgr.list_configured_servers();

    using daedalus::concurrent::ThreadPool;
    using daedalus::concurrent::TaskPriority;

    static constexpr size_t kMaxParallelConnects = 5;
    std::vector<std::future<bool>> futures;
    futures.reserve(configured.size());
    std::atomic<size_t> active{0};

    for (const auto& server_name : configured) {
        // Already connected? Skip the spawn — it's expensive.
        if (mgr.is_connected(server_name)) continue;

        // Throttle: at most 5 concurrent connects.
        // We spin-wait — these are short, and there's no async-friendly
        // semaphore in the pool yet.
        while (active.load() >= kMaxParallelConnects) {
            std::this_thread::yield();
        }
        ++active;

        futures.push_back(ThreadPool::get_instance().submit(
            TaskPriority::NORMAL,
            [&mgr, server_name, &active]() -> bool {
                bool ok = mgr.connect(server_name);
                --active;
                return ok;
            }));
    }
    // Wait for all to finish (success or breaker-recorded failure).
    for (auto& f : futures) {
        try { f.get(); } catch (...) {
            // Manager catches its own exceptions and updates the breaker.
        }
    }

    // 3) For every server that now reports healthy, register one
    // McpProxiedTool per discovered tool. We re-read list_configured_
    // servers() at this point in case the manager mutated it (e.g. list_
    // changed happened during connect).
    json schema_dumped = json::array();
    for (const auto& server_name : mgr.list_configured_servers()) {
        if (!mgr.is_healthy(server_name)) continue;
        const auto& tools = mgr.get_tools(server_name);
        for (const auto& def : tools) {
            // §Phase 11: pass mcp_manager_ to each McpProxiedTool
            // (injected instead of ::instance()).
            auto tool = std::make_shared<McpProxiedTool>(
                server_name, def, mcp_manager_);
            schema_dumped.push_back(tool->get_definition());
            register_tool(std::move(tool));
        }
    }

    // 4) §9.7.C — publish the schema snapshot so callers (LLMClient,
    // /mcp-tools command, token-warning HUD) can measure.
    mgr.set_schema_dumped_for_test(schema_dumped.dump());
    mgr.clear_mcp_tools_dirty();
}
bool ToolFactory::has_tool(const std::string& tool_name) const {
    for (const auto& tool : m_tools) {
        if (tool->get_name() == tool_name) {
            return true;
        }
    }
    return false;
}
std::shared_ptr<BaseTool> ToolFactory::get_tool(const std::string& tool_name) const {
    for (const auto& tool : m_tools) {
        if (tool->get_name() == tool_name) {
            return tool;
        }
    }
    return nullptr;
}
std::vector<std::string> ToolFactory::get_tool_names() const {
    std::vector<std::string> names;
    for (const auto& tool : m_tools) {
        names.push_back(tool->get_name());
    }
    return names;
}
