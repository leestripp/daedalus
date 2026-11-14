// SPDX-License-Identifier: BSL-1.0
#include "SessionCatalog.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace monkeybyte {
namespace {

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string new_id() {
    std::random_device random;
    std::ostringstream out;
    out << std::hex;
    for (int index = 0; index < 16; ++index) out << std::setw(2) << std::setfill('0') << (random() & 0xffU);
    return out.str();
}

void require_name(std::string_view name) {
    if (name.empty() || name.size() > 120) throw std::invalid_argument("Session name must contain 1–120 bytes.");
}

}  // namespace

nlohmann::json SessionRecord::to_json() const {
    return {{"id", id}, {"name", name}, {"workspace", workspace.string()},
            {"model", model}, {"thinking", thinking}, {"createdAt", created_at},
            {"updatedAt", updated_at}};
}

void to_json(nlohmann::json& value, const SessionRecord& record) {
    value = record.to_json();
}

SessionRecord SessionRecord::from_json(const nlohmann::json& value) {
    SessionRecord result;
    result.id = value.at("id").get<std::string>();
    result.name = value.at("name").get<std::string>();
    result.workspace = value.at("workspace").get<std::string>();
    result.model = value.value("model", "");
    result.thinking = value.value("thinking", "");
    result.created_at = value.value("createdAt", "");
    result.updated_at = value.value("updatedAt", "");
    return result;
}

SessionCatalog::SessionCatalog(std::filesystem::path index_path)
    : index_path_(std::move(index_path)) {
    load();
}

void SessionCatalog::load() {
    std::lock_guard lock(mutex_);
    if (!std::filesystem::exists(index_path_)) return;
    std::ifstream input(index_path_);
    if (!input) throw std::runtime_error("Cannot read the session index.");
    const auto value = nlohmann::json::parse(input);
    sessions_.clear();
    for (const auto& item : value.at("sessions")) sessions_.push_back(SessionRecord::from_json(item));
    active_id_ = value.value("activeId", "");
}

void SessionCatalog::save_locked() const {
    std::filesystem::create_directories(index_path_.parent_path());
    const nlohmann::json value = {{"version", 1}, {"activeId", active_id_},
                                  {"sessions", sessions_}};
    const auto temporary = index_path_;
    auto path = temporary;
    path += ".tmp";
    {
        std::ofstream output(path, std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write the session index.");
        output << value.dump(2) << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Cannot flush the session index.");
    }
    std::filesystem::rename(path, index_path_);
}

SessionRecord* SessionCatalog::find_locked(std::string_view id) {
    const auto found = std::ranges::find_if(sessions_, [&](const auto& item) { return item.id == id; });
    return found == sessions_.end() ? nullptr : &*found;
}

const SessionRecord* SessionCatalog::find_locked(std::string_view id) const {
    const auto found = std::ranges::find_if(sessions_, [&](const auto& item) { return item.id == id; });
    return found == sessions_.end() ? nullptr : &*found;
}

nlohmann::json SessionCatalog::list() const {
    std::lock_guard lock(mutex_);
    return {{"activeId", active_id_}, {"sessions", sessions_}};
}

std::optional<SessionRecord> SessionCatalog::active() const {
    std::lock_guard lock(mutex_);
    if (const auto* record = find_locked(active_id_)) return *record;
    return std::nullopt;
}


SessionRecord SessionCatalog::ensure(const std::filesystem::path& workspace,
                                     std::string_view requested_id) {
    std::lock_guard lock(mutex_);
    if (!requested_id.empty()) {
        auto* requested = find_locked(requested_id);
        if (!requested) throw std::invalid_argument("Unknown session.");
        active_id_ = requested->id;
        save_locked();
        return *requested;
    }
    if (auto* current = find_locked(active_id_); current && current->workspace == workspace) return *current;
    const auto found = std::ranges::find_if(sessions_, [&](const auto& item) { return item.workspace == workspace; });
    if (found != sessions_.end()) {
        active_id_ = found->id;
        save_locked();
        return *found;
    }
    const auto now = utc_now();
    SessionRecord record{.id = new_id(), .name = "New Chat", .workspace = workspace,
                         .created_at = now, .updated_at = now};
    sessions_.push_back(record);
    active_id_ = record.id;
    save_locked();
    return record;
}

SessionRecord SessionCatalog::create(const std::filesystem::path& workspace, std::string name) {
    require_name(name);
    std::lock_guard lock(mutex_);
    const auto now = utc_now();
    SessionRecord record{.id = new_id(), .name = std::move(name), .workspace = workspace,
                         .created_at = now, .updated_at = now};
    sessions_.push_back(record);
    active_id_ = record.id;
    save_locked();
    return record;
}

SessionRecord SessionCatalog::select(std::string_view id) {
    std::lock_guard lock(mutex_);
    auto* record = find_locked(id);
    if (!record) throw std::invalid_argument("Unknown session.");
    active_id_ = record->id;
    save_locked();
    return *record;
}

SessionRecord SessionCatalog::rename(std::string_view id, std::string name) {
    require_name(name);
    std::lock_guard lock(mutex_);
    auto* record = find_locked(id);
    if (!record) throw std::invalid_argument("Unknown session.");
    record->name = std::move(name);
    record->updated_at = utc_now();
    save_locked();
    return *record;
}

SessionRecord SessionCatalog::set_model(std::string_view id, std::string model) {
    std::lock_guard lock(mutex_);
    auto* record = find_locked(id);
    if (!record) throw std::invalid_argument("Unknown session.");
    record->model = std::move(model);
    record->updated_at = utc_now();
    save_locked();
    return *record;
}

SessionRecord SessionCatalog::set_thinking(std::string_view id, std::string thinking) {
    std::lock_guard lock(mutex_);
    auto* record = find_locked(id);
    if (!record) throw std::invalid_argument("Unknown session.");
    record->thinking = std::move(thinking);
    record->updated_at = utc_now();
    save_locked();
    return *record;
}

SessionRecord SessionCatalog::remove(std::string_view id) {
    std::lock_guard lock(mutex_);
    const auto found = std::ranges::find_if(sessions_, [&](const auto& item) { return item.id == id; });
    if (found == sessions_.end()) throw std::invalid_argument("Unknown session.");
    const auto removed = *found;
    sessions_.erase(found);
    if (active_id_ == id) active_id_ = sessions_.empty() ? "" : sessions_.front().id;
    save_locked();
    return removed;
}

}  // namespace monkeybyte
