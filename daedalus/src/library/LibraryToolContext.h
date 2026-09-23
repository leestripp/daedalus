/**
 * @file LibraryToolContext.h
 * @brief Internal adapter that exposes the library's FileIO through the
 *        public HostToolContext interface. Implementation lives in
 *        AgentImpl.cpp (Phase 2 will back it with the real FileIO).
 */
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "daedalus/types/HostToolContext.h"
#include "daedalus/types/AgentConfig.h"

namespace daedalus::detail {

class LibraryToolContext : public daedalus::HostToolContext {
public:
    explicit LibraryToolContext(daedalus::AgentConfig cfg);

    std::string read_file(const std::filesystem::path& p) override;
    void        write_file(const std::filesystem::path& p,
                           const std::string& content) override;
    std::vector<std::string>
        list_directory(const std::filesystem::path& p) override;

    [[nodiscard]] const std::filesystem::path& project_root() const override
    {
        return cfg_.project_root;
    }

private:
    daedalus::AgentConfig cfg_;
};

}  // namespace daedalus::detail