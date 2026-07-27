// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/McpTab.cpp
//
// MCP servers tab: list / add / edit / delete SettingsStore::McpServer
// rows. Stdio servers edit command/args/env; HTTP servers edit
// url/bearer_token. All fields stay visible in the form — the transport
// radiobox decides which ones are persisted as meaningful.

#include "SettingsApp.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace settings_tui {

using namespace ftxui;

namespace {

const std::vector<std::string> kTransports = {"stdio", "streamable-http"};

struct McpState
{
    std::vector<std::string> names;
    int selected = 0;

    // Edit form.
    bool show_form = false;
    bool editing   = false;
    std::string original_name;
    std::string f_name;
    int transport_index = 0;
    std::string f_command;
    std::string f_args;
    std::string f_env;
    std::string f_url;
    std::string f_bearer;
    std::string f_description;
    bool f_enabled = true;
    std::string form_error;

    // Delete confirmation.
    bool confirm_delete = false;
};

} // namespace

Component MakeMcpTab(AppContext& ctx)
{
    auto st = std::make_shared<McpState>();

    auto refresh = [&ctx, st] {
        st->names.clear();
        for (const auto& s : ctx.store.list_mcp_servers())
            st->names.push_back(s.name);
        if (st->selected >= static_cast<int>(st->names.size()))
            st->selected = std::max(0, static_cast<int>(st->names.size()) - 1);
    };
    refresh();

    // -- form open helpers --------------------------------------------------
    auto open_add = [st] {
        st->editing = false;
        st->original_name.clear();
        st->f_name.clear();
        st->transport_index = 0;
        st->f_command.clear();
        st->f_args.clear();
        st->f_env.clear();
        st->f_url.clear();
        st->f_bearer.clear();
        st->f_description.clear();
        st->f_enabled = true;
        st->form_error.clear();
        st->show_form = true;
    };

    auto open_edit = [&ctx, st] {
        if (st->names.empty()) return;
        auto s = ctx.store.get_mcp_server(st->names[st->selected]);
        if (!s) return;
        st->editing       = true;
        st->original_name = s->name;
        st->f_name        = s->name;
        st->transport_index = (s->transport == "streamable-http") ? 1 : 0;
        st->f_command     = s->command;
        st->f_args        = JoinWords(s->args);
        st->f_env         = EnvToLines(s->env);
        st->f_url         = s->url;
        st->f_bearer      = s->bearer_token;
        st->f_description = s->description;
        st->f_enabled     = s->enabled;
        st->form_error.clear();
        st->show_form = true;
    };

    // -- form save ----------------------------------------------------------
    auto save = [&ctx, st, refresh] {
        if (st->f_name.empty()) {
            st->form_error = "name must not be empty";
            return;
        }
        daedalus::SettingsStore::McpServer s;
        s.name         = st->f_name;
        s.transport    = kTransports[st->transport_index];
        s.command      = st->f_command;
        s.args         = SplitWords(st->f_args);
        s.env          = ParseEnvLines(st->f_env);
        s.url          = st->f_url;
        s.bearer_token = st->f_bearer;
        s.description  = st->f_description;
        s.enabled      = st->f_enabled;

        ctx.store.upsert_mcp_server(s);
        if (st->editing && st->original_name != s.name)
            ctx.store.delete_mcp_server(st->original_name);

        refresh();
        ctx.set_status("MCP server '" + s.name + "' saved");
        st->show_form = false;
    };

    // -- delete -------------------------------------------------------------
    auto do_delete = [&ctx, st, refresh] {
        if (st->names.empty()) return;
        const std::string name = st->names[st->selected];
        ctx.store.delete_mcp_server(name);
        refresh();
        ctx.set_status("MCP server '" + name + "' deleted");
        st->confirm_delete = false;
    };

    // -- components ---------------------------------------------------------
    MenuOption menu_opt = MenuOption::Vertical();
    menu_opt.entries    = &st->names;
    menu_opt.selected   = &st->selected;
    menu_opt.on_enter   = open_edit;
    auto menu = Menu(menu_opt);

    auto btn_add  = Button(" Add (a) ", open_add, ButtonOption::Border());
    auto btn_edit = Button(" Edit (e) ", open_edit, ButtonOption::Border());
    auto btn_del  = Button(" Delete (d) ",
                           [st] { if (!st->names.empty()) st->confirm_delete = true; },
                           ButtonOption::Border());
    auto buttons = Container::Horizontal({btn_add, btn_edit, btn_del});

    auto body = Container::Vertical({menu, buttons});

    auto body_renderer = Renderer(body, [&ctx, st, menu, btn_add, btn_edit, btn_del] {
        Elements detail;
        detail.push_back(text("MCP server details") | bold);
        detail.push_back(separator());
        if (st->names.empty()) {
            detail.push_back(text("(no MCP servers — press 'a' to add one)") | dim);
        } else if (auto s = ctx.store.get_mcp_server(st->names[st->selected])) {
            detail.push_back(hbox({text("name         : "), text(s->name),
                                   s->enabled ? text("") : text("  (disabled)") | color(Color::Red)}));
            detail.push_back(hbox({text("transport    : "), text(s->transport)}));
            if (s->transport == "stdio") {
                detail.push_back(hbox({text("command      : "), text(s->command)}));
                detail.push_back(hbox({text("args         : "), text(JoinWords(s->args))}));
                for (const auto& [k, v] : s->env)
                    detail.push_back(hbox({text("env." + k + " : "), text(MaskSecret(v))}));
            } else {
                detail.push_back(hbox({text("url          : "), text(s->url)}));
                detail.push_back(hbox({text("bearer_token : "), text(MaskSecret(s->bearer_token))}));
            }
            if (!s->description.empty())
                detail.push_back(hbox({text("description  : "), text(s->description)}));
        }
        return hbox({
            vbox({
                text(" mcp servers ") | bold,
                separator(),
                menu->Render() | frame | flex,
            }) | border | size(WIDTH, GREATER_THAN, 28),
            vbox({
                vbox(std::move(detail)) | flex,
                separator(),
                hbox({btn_add->Render(), btn_edit->Render(), btn_del->Render()}),
            }) | border | flex,
        });
    });

    // -- edit form modal ----------------------------------------------------
    auto row_name    = FormRow("name", &st->f_name, false, save);
    auto row_command = FormRow("command", &st->f_command, false, save);
    auto row_args    = FormRow("args (space-sep.)", &st->f_args, false, save);
    auto row_url     = FormRow("url", &st->f_url, false, save);
    auto row_bearer  = FormRow("bearer_token", &st->f_bearer, /*password=*/true, save);
    auto row_desc    = FormRow("description", &st->f_description, false, save);
    auto transport_radio = Radiobox(&kTransports, &st->transport_index);
    auto enabled_check   = Checkbox("enabled", &st->f_enabled);

    InputOption env_opt;
    env_opt.content   = StringRef(&st->f_env);
    env_opt.multiline = true;
    auto env_input = Input(env_opt);
    auto env_renderer = Renderer(env_input, [env_input] {
        return vbox({
            text("env (KEY=VALUE, one per line)"),
            env_input->Render() | frame | size(HEIGHT, EQUAL, 4),
        });
    });

    auto btn_save   = Button(" Save ", save, ButtonOption::Border());
    auto btn_cancel = Button(" Cancel ", [st] { st->show_form = false; }, ButtonOption::Border());
    auto form = Container::Vertical({
        Container::Vertical({row_name, transport_radio, row_command, row_args,
                             env_renderer, row_url, row_bearer, row_desc,
                             enabled_check}),
        Container::Horizontal({btn_save, btn_cancel}),
    });

    auto form_modal = Renderer(form,
        [st, row_name, transport_radio, row_command, row_args, env_renderer,
         row_url, row_bearer, row_desc, enabled_check, btn_save, btn_cancel] {
            Elements rows;
            rows.push_back(text(st->editing ? " Edit MCP server " : " Add MCP server ") | bold);
            rows.push_back(separator());
            rows.push_back(row_name->Render());
            rows.push_back(hbox({text("transport") | size(WIDTH, EQUAL, 24),
                                 transport_radio->Render()}));
            rows.push_back(row_command->Render());
            rows.push_back(row_args->Render());
            rows.push_back(env_renderer->Render());
            rows.push_back(row_url->Render());
            rows.push_back(row_bearer->Render());
            rows.push_back(row_desc->Render());
            rows.push_back(enabled_check->Render());
            if (!st->form_error.empty())
                rows.push_back(text(" " + st->form_error) | color(Color::Red));
            rows.push_back(separator());
            rows.push_back(hbox({btn_save->Render(), btn_cancel->Render()}) | center);
            return vbox(std::move(rows)) | border | size(WIDTH, GREATER_THAN, 64) | clear_under;
        }) | CatchEvent([st](Event e) {
            if (e == Event::Escape) {
                st->show_form = false;
                return true;
            }
            return false;
        });

    // -- delete confirmation modal ------------------------------------------
    auto btn_yes = Button(" Delete ", do_delete, ButtonOption::Border());
    auto btn_no  = Button(" Cancel ", [st] { st->confirm_delete = false; }, ButtonOption::Border());
    auto confirm_buttons = Container::Horizontal({btn_yes, btn_no});
    auto confirm_modal = Renderer(confirm_buttons, [st, btn_yes, btn_no] {
        const std::string name =
            st->names.empty() ? std::string{} : st->names[st->selected];
        return vbox({
            text(" Delete MCP server ") | bold,
            separator(),
            text("Really delete '" + name + "'?"),
            separator(),
            hbox({btn_yes->Render(), btn_no->Render()}) | center,
        }) | border | clear_under | center;
    }) | CatchEvent([st](Event e) {
        if (e == Event::Escape) {
            st->confirm_delete = false;
            return true;
        }
        return false;
    });

    // -- keyboard shortcuts (only when no modal is open) --------------------
    // Registered on the shared context; the root CatchEvent routes events
    // for the active tab here (see SettingsApp.h).
    ctx.key_handlers.emplace_back([st, open_add, open_edit](Event e) {
        if (st->show_form || st->confirm_delete) return false;
        if (e == Event::Character('a')) { open_add(); return true; }
        if (e == Event::Character('e')) { open_edit(); return true; }
        if (e == Event::Character('d')) {
            if (!st->names.empty()) st->confirm_delete = true;
            return true;
        }
        return false;
    });
    ctx.modal_checks.emplace_back([st] {
        return st->show_form || st->confirm_delete;
    });

    return body_renderer
         | Modal(form_modal, &st->show_form)
         | Modal(confirm_modal, &st->confirm_delete);
}

} // namespace settings_tui
