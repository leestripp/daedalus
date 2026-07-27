// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/AgentTab.cpp
//
// Agent tab: typed form over the kv_settings-backed agent options
// (auto_approve, size limits, MCP soft cap, Serper key, current provider)
// plus the server_error retry/backoff group. Values are loaded on open
// and written back with the "Save" button.

#include "SettingsApp.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace settings_tui {

using namespace ftxui;

namespace {

struct AgentState
{
    bool auto_approve = false;
    std::string current_provider;
    std::string max_agent_iterations;
    std::string max_tool_result_size;
    std::string max_request_size;
    std::string max_response_size;
    std::string soft_cap_percent;
    std::string serper_api_key;

    bool se_enabled = true;
    std::string se_initial_wait;
    std::string se_max_wait;
    std::string se_backoff;
    std::string se_max_retries;

    std::string form_error;
};

} // namespace

Component MakeAgentTab(AppContext& ctx)
{
    auto st = std::make_shared<AgentState>();

    // No letter shortcuts on this tab (its Inputs are always focusable),
    // but register no-ops so the handler/check vectors stay aligned with
    // tab order.
    ctx.key_handlers.emplace_back([](Event) { return false; });
    ctx.modal_checks.emplace_back([] { return false; });

    auto load = [&ctx, st] {
        st->auto_approve         = ctx.store.get_auto_approve();
        st->current_provider     = ctx.store.get_current_provider();
        st->max_agent_iterations = std::to_string(ctx.store.get_max_agent_iterations());
        st->max_tool_result_size = std::to_string(ctx.store.get_max_tool_result_size());
        st->max_request_size     = std::to_string(ctx.store.get_max_request_size());
        st->max_response_size    = std::to_string(ctx.store.get_max_response_size());
        st->soft_cap_percent     = std::to_string(ctx.store.get_mcp_tool_schema_soft_cap_percent());
        st->serper_api_key       = ctx.store.get_serper_api_key();

        const auto ses = ctx.store.get_server_error_settings();
        st->se_enabled      = ses.enabled;
        st->se_initial_wait = std::to_string(ses.initial_wait_seconds);
        st->se_max_wait     = std::to_string(ses.max_wait_seconds);
        st->se_backoff      = std::to_string(ses.backoff_multiplier);
        st->se_max_retries  = std::to_string(ses.max_retries_per_call);

        st->form_error.clear();
    };
    load();

    auto save = [&ctx, st] {
        int max_iter = 0, soft_cap = 0, se_initial = 0, se_max = 0, se_retries = 0;
        size_t tool_result = 0, request = 0, response = 0;
        double backoff = 0.0;

        if (!ParseInt(st->max_agent_iterations, max_iter) || max_iter <= 0)
            { st->form_error = "max_agent_iterations must be a positive integer"; return; }
        if (!ParseSize(st->max_tool_result_size, tool_result))
            { st->form_error = "max_tool_result_size must be a non-negative integer"; return; }
        if (!ParseSize(st->max_request_size, request))
            { st->form_error = "max_request_size must be a non-negative integer"; return; }
        if (!ParseSize(st->max_response_size, response))
            { st->form_error = "max_response_size must be a non-negative integer"; return; }
        if (!ParseInt(st->soft_cap_percent, soft_cap) || soft_cap < 0 || soft_cap > 100)
            { st->form_error = "soft cap percent must be an integer in [0, 100]"; return; }
        if (!ParseInt(st->se_initial_wait, se_initial) || se_initial < 0)
            { st->form_error = "server_error initial_wait_seconds must be a non-negative integer"; return; }
        if (!ParseInt(st->se_max_wait, se_max) || se_max < 0)
            { st->form_error = "server_error max_wait_seconds must be a non-negative integer"; return; }
        if (!ParseDouble(st->se_backoff, backoff) || backoff < 1.0)
            { st->form_error = "server_error backoff_multiplier must be a number >= 1.0"; return; }
        if (!ParseInt(st->se_max_retries, se_retries) || se_retries < 0)
            { st->form_error = "server_error max_retries_per_call must be a non-negative integer"; return; }

        ctx.store.set_auto_approve(st->auto_approve);
        ctx.store.set_current_provider(st->current_provider);
        ctx.store.set_max_agent_iterations(max_iter);
        ctx.store.set_max_tool_result_size(tool_result);
        ctx.store.set_max_request_size(request);
        ctx.store.set_max_response_size(response);
        ctx.store.set_mcp_tool_schema_soft_cap_percent(soft_cap);
        ctx.store.set_serper_api_key(st->serper_api_key);

        // server_error has no typed setter — persist via the same kv keys
        // SettingsStore::get_server_error_settings() reads.
        ctx.store.set_bool ("server_error_enabled", st->se_enabled);
        ctx.store.set_int  ("server_error_initial_wait_seconds", se_initial);
        ctx.store.set_int  ("server_error_max_wait_seconds", se_max);
        ctx.store.set_string("server_error_backoff_multiplier", st->se_backoff);
        ctx.store.set_int  ("server_error_max_retries_per_call", se_retries);

        st->form_error.clear();
        ctx.set_status("agent settings saved");
    };

    // -- components ---------------------------------------------------------
    auto row_provider   = FormRow("current_provider", &st->current_provider, false, save);
    auto row_max_iter   = FormRow("max_agent_iterations", &st->max_agent_iterations, false, save);
    auto row_tool_size  = FormRow("max_tool_result_size", &st->max_tool_result_size, false, save);
    auto row_req_size   = FormRow("max_request_size", &st->max_request_size, false, save);
    auto row_res_size   = FormRow("max_response_size", &st->max_response_size, false, save);
    auto row_soft_cap   = FormRow("mcp soft cap %", &st->soft_cap_percent, false, save);
    auto row_serper     = FormRow("serper_api_key", &st->serper_api_key, /*password=*/true, save);
    auto approve_check  = Checkbox("auto_approve (skip bash confirmations)", &st->auto_approve);

    auto se_enabled_check = Checkbox("server_error retry enabled", &st->se_enabled);
    auto row_se_initial   = FormRow("initial_wait_seconds", &st->se_initial_wait, false, save);
    auto row_se_max       = FormRow("max_wait_seconds", &st->se_max_wait, false, save);
    auto row_se_backoff   = FormRow("backoff_multiplier", &st->se_backoff, false, save);
    auto row_se_retries   = FormRow("max_retries_per_call", &st->se_max_retries, false, save);

    auto btn_save   = Button(" Save ", save, ButtonOption::Border());
    auto btn_reload = Button(" Reload ", [load] { load(); }, ButtonOption::Border());

    auto form = Container::Vertical({
        approve_check,
        row_provider, row_max_iter, row_tool_size, row_req_size, row_res_size,
        row_soft_cap, row_serper,
        se_enabled_check, row_se_initial, row_se_max, row_se_backoff, row_se_retries,
        Container::Horizontal({btn_save, btn_reload}),
    });

    return Renderer(form,
        [st, approve_check, row_provider, row_max_iter, row_tool_size, row_req_size,
         row_res_size, row_soft_cap, row_serper, se_enabled_check, row_se_initial,
         row_se_max, row_se_backoff, row_se_retries, btn_save, btn_reload] {
            Elements rows;
            rows.push_back(text(" agent settings ") | bold);
            rows.push_back(separator());
            rows.push_back(approve_check->Render());
            rows.push_back(row_provider->Render());
            rows.push_back(row_max_iter->Render());
            rows.push_back(row_tool_size->Render());
            rows.push_back(row_req_size->Render());
            rows.push_back(row_res_size->Render());
            rows.push_back(row_soft_cap->Render());
            rows.push_back(row_serper->Render());
            rows.push_back(separator());
            rows.push_back(text(" server error retry ") | bold);
            rows.push_back(separator());
            rows.push_back(se_enabled_check->Render());
            rows.push_back(row_se_initial->Render());
            rows.push_back(row_se_max->Render());
            rows.push_back(row_se_backoff->Render());
            rows.push_back(row_se_retries->Render());
            if (!st->form_error.empty())
                rows.push_back(text(" " + st->form_error) | color(Color::Red));
            rows.push_back(separator());
            rows.push_back(hbox({btn_save->Render(), btn_reload->Render()}));
            return vbox(std::move(rows)) | border | flex;
        });
}

} // namespace settings_tui
