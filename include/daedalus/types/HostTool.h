/**
 * @file daedalus/HostTool.h
 * @brief Base class for tools registered by the host application.
 *
 * Most tools live inside the library. Hosts can extend the agent's toolbox
 * without modifying libdaedalus by subclassing HostTool and registering with
 * Agent::register_tool().
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>

#include "daedalus/types/HostToolContext.h"

namespace daedalus {

class HostTool {
public:
    virtual ~HostTool() = default;

    [[nodiscard]] virtual std::string get_name() const = 0;
    [[nodiscard]] virtual std::string get_description() const = 0;
    [[nodiscard]] virtual nlohmann::json get_definition() const = 0;
    virtual nlohmann::json execute(const nlohmann::json& args,
                                   HostToolContext& ctx) = 0;
};

}  // namespace daedalus