// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/ProvidersTab.cpp
//
// Providers tab: list / add / edit / delete ProviderConfig rows via
// SettingsStore::list_providers_detailed() / upsert_provider() /
// delete_provider().

#include "SettingsApp.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace settings_tui {

using namespace ftxui;

namespace {

struct ProvidersState
{
    std::vector<std::string> names;
    int selected = 0;

    // Edit form.
    bool show_form = false;
    bool editing   = false;
    std::string original_name;
    std::string f_name;
    std::string f_host;
    std::string f_api_key;
    std::string f_chat_endpoint;
    std::string f_discovery_endpoint;
    std::string f_timeout;
    std::string form_error;

    // Delete confirmation.
    bool confirm_delete = false;
};

} // namespace

Component MakeProvidersTab(AppContext& ctx)
{
    auto st = std::make_shared<ProvidersState>();

    auto refresh = [&ctx, st] {
        st->names = ctx.store.list_providers();
        if (st->selected >= static_cast<int>(st->names.size()))
            st->selected = std::max(0, static_cast<int>(st->names.size()) - 1);
    };
    refresh();

    // -- form open helpers --------------------------------------------------
    auto open_add = [st] {
        st->editing = false;
        st->original_name.clear();
        st->f_name.clear();
        st->f_host.clear();
        st->f_api_key.clear();
        st->f_chat_endpoint      = "/v1/chat/completions";
        st->f_discovery_endpoint = "/v1/models";
        st->f_timeout            = "30";
        st->form_error.clear();
        st->show_form = true;
    };

    auto open_edit = [&ctx, st] {
        if (st->names.empty()) return;
        auto p = ctx.store.get_provider(st->names[st->selected]);
        if (!p) return;
        st->editing            = true;
        st->original_name      = p->name;
        st->f_name             = p->name;
        st->f_host             = p->host;
        st->f_api_key          = p->api_key;
        st->f_chat_endpoint    = p->chat_endpoint;
        st->f_discovery_endpoint = p->discovery_endpoint;
        st->f_timeout          = std::to_string(p->timeout_seconds);
        st->form_error.clear();
        st->show_form = true;
    };

    // -- form save ----------------------------------------------------------
    auto save = [&ctx, st, refresh] {
        if (st->f_name.empty()) {
            st->form_error = "name must not be empty";
            return;
        }
        int timeout = 0;
        if (!ParseInt(st->f_timeout, timeout) || timeout <= 0) {
            st->form_error = "timeout must be a positive integer";
            return;
        }
        daedalus::ProviderConfig p;
        p.name               = st->f_name;
        p.host               = st->f_host;
        p.api_key            = st->f_api_key;
        p.chat_endpoint      = st->f_chat_endpoint;
        p.discovery_endpoint = st->f_discovery_endpoint;
        p.timeout_seconds    = timeout;

        ctx.store.upsert_provider(p);
        if (st->editing && st->original_name != p.name)
            ctx.store.delete_provider(st->original_name);

        refresh();
        ctx.set_status("provider '" + p.name + "' saved");
        st->show_form = false;
    };

    // -- delete -------------------------------------------------------------
    auto do_delete = [&ctx, st, refresh] {
        if (st->names.empty()) return;
        const std::string name = st->names[st->selected];
        ctx.store.delete_provider(name);
        refresh();
        ctx.set_status("provider '" + name + "' deleted");
        st->confirm_delete = false;
    };

    // -- components ---------------------------------------------------------
    MenuOption menu_opt = MenuOption::Vertical();
    menu_opt.entries    = &st->names;
    menu_opt.selected   = &st->selected;
    menu_opt.on_enter   = open_edit;
    auto menu = Menu(menu_opt);

    auto btn_add = Button(" Add (a) ", open_add, ButtonOption::Border());
    auto btn_edit = Button(" Edit (e) ", open_edit, ButtonOption::Border());
    auto btn_del = Button(" Delete (d) ",
                          [st] { if (!st->names.empty()) st->confirm_delete = true; },
                          ButtonOption::Border());
    auto buttons = Container::Horizontal({btn_add, btn_edit, btn_del});

    auto body = Container::Vertical({menu, buttons});

    auto body_renderer = Renderer(body, [&ctx, st, menu, btn_add, btn_edit, btn_del] {
        Elements detail;
        detail.push_back(text("provider details") | bold);
        detail.push_back(separator());
        if (st->names.empty()) {
            detail.push_back(text("(no providers — press 'a' to add one)") | dim);
        } else if (auto p = ctx.store.get_provider(st->names[st->selected])) {
            detail.push_back(hbox({text("name               : "), text(p->name)}));
            detail.push_back(hbox({text("host               : "), text(p->host)}));
            detail.push_back(hbox({text("api_key            : "), text(MaskSecret(p->api_key))}));
            detail.push_back(hbox({text("chat_endpoint      : "), text(p->chat_endpoint)}));
            detail.push_back(hbox({text("discovery_endpoint : "), text(p->discovery_endpoint)}));
            detail.push_back(hbox({text("timeout_seconds    : "), text(std::to_string(p->timeout_seconds))}));
        }
        return hbox({
            vbox({
                text(" providers ") | bold,
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
    auto row_name      = FormRow("name", &st->f_name, false, save);
    auto row_host      = FormRow("host", &st->f_host, false, save);
    auto row_api_key   = FormRow("api_key", &st->f_api_key, /*password=*/true, save);
    auto row_chat      = FormRow("chat_endpoint", &st->f_chat_endpoint, false, save);
    auto row_discovery = FormRow("discovery_endpoint", &st->f_discovery_endpoint, false, save);
    auto row_timeout   = FormRow("timeout_seconds", &st->f_timeout, false, save);
    Components rows = {row_name, row_host, row_api_key, row_chat, row_discovery, row_timeout};

    auto btn_save   = Button(" Save ", save, ButtonOption::Border());
    auto btn_cancel = Button(" Cancel ", [st] { st->show_form = false; }, ButtonOption::Border());
    auto form = Container::Vertical({
        Container::Vertical(rows),
        Container::Horizontal({btn_save, btn_cancel}),
    });

    auto form_modal = Renderer(form, [st, rows, btn_save, btn_cancel] {
        Elements rendered;
        rendered.push_back(text(st->editing ? " Edit provider " : " Add provider ") | bold);
        rendered.push_back(separator());
        for (const auto& row : rows)
            rendered.push_back(row->Render());
        if (!st->form_error.empty())
            rendered.push_back(text(" " + st->form_error) | color(Color::Red));
        rendered.push_back(separator());
        rendered.push_back(hbox({btn_save->Render(), btn_cancel->Render()}) | center);
        return vbox(std::move(rendered)) | border | size(WIDTH, GREATER_THAN, 60) | clear_under;
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
            text(" Delete provider ") | bold,
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
