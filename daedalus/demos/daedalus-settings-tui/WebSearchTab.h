// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/WebSearchTab.h
#pragma once

#include "SettingsApp.h"

namespace settings_tui {

/// §WEB-SEARCH-CORE (2026-10-10): Web Search tab — one block per
/// web_search provider (enable toggle, API key or instance URL +
/// credentials, priority). Maps to the `web_search_providers` table
/// (schema v8); the unified web_search tool reads this per call, so
/// edits apply immediately (no restart).
ftxui::Component MakeWebSearchTab(AppContext& ctx);

} // namespace settings_tui
