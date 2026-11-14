// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace monkeybyte {

struct SessionRecord {
    std::string id;
    std::string name;
    std::filesystem::path workspace;
    std::string model;
    std::string thinking;
    std::string created_at;
    std::string updated_at;

    [[nodiscard]] nlohmann::json to_json() const;
    static SessionRecord from_json(const nlohmann::json& value);
};

void to_json(nlohmann::json& value, const SessionRecord& record);

/// Small durable index; conversation bodies remain owned by libdaedalus.
class SessionCatalog {
public:
    explicit SessionCatalog(std::filesystem::path index_path);

    [[nodiscard]] nlohmann::json list() const;
    [[nodiscard]] SessionRecord ensure(const std::filesystem::path& workspace,
                                       std::string_view requested_id = {});
    [[nodiscard]] SessionRecord create(const std::filesystem::path& workspace,
                                       std::string name);
    [[nodiscard]] SessionRecord select(std::string_view id);
    [[nodiscard]] SessionRecord rename(std::string_view id, std::string name);
    [[nodiscard]] SessionRecord set_model(std::string_view id, std::string model);
    [[nodiscard]] SessionRecord set_thinking(std::string_view id, std::string thinking);
    [[nodiscard]] std::optional<SessionRecord> active() const;
    [[nodiscard]] SessionRecord remove(std::string_view id);

private:
    std::filesystem::path index_path_;
    mutable std::mutex mutex_;
    std::vector<SessionRecord> sessions_;
    std::string active_id_;

    void load();
    void save_locked() const;
    [[nodiscard]] SessionRecord* find_locked(std::string_view id);
    [[nodiscard]] const SessionRecord* find_locked(std::string_view id) const;
};

}  // namespace monkeybyte
