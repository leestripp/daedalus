// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "BaseTool.h"

class ScheduleTaskTool final : public BaseTool {
public:
    std::string get_name() const override { return "schedule_task"; }
    std::string get_description() const override {
        return "Add, list, edit, or delete persistent time-based agent tasks. "
               "One-shot tasks fire once and remove themselves; recurring tasks "
               "fire at a fixed interval. Tasks run while the host is active; "
               "overdue tasks fire on startup.";
    }
    nlohmann::json get_definition() const override;
    nlohmann::json execute(const nlohmann::json& args, FileIO& file_io) override;
};
