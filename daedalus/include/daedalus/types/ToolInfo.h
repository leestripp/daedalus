/**
 * @file daedalus/ToolInfo.h
 * @brief Read-only description of a tool the agent has registered.
 */
#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace daedalus {

struct ToolInfo {
    std::string name;
    std::string description;
    nlohmann::json schema;  ///< Raw JSON-schema for arguments
};

}  // namespace daedalus