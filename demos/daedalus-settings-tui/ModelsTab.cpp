// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/ModelsTab.cpp
//
// Models tab: list / add / edit / delete ModelConfig rows, mark the
// default model, and discover model ids from the provider's
// OpenAI-compatible /v1/models endpoint.

#include "SettingsApp.h"

#include <daedalus/model_discovery.h>

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

namespace settings_tui {

using namespace ftxui;

namespace {

struct ModelsState
{
    std::vector<std::string> names;
    int selected = 0;
    std::string default_name;

    // Edit form.
    bool show_form = false;
    bool editing   = false;
    std::string original_name;
    std::string f_name;
    std::string f_model_id;
    std::string f_max_ctx;
    std::string f_temperature;
    std::vector<std::string> provider_names;
    int provider_index = 0;
    std::string form_error;

    // Delete confirmation.
    bool confirm_delete = false;

    // Discovery chooser.
    bool show_discover = false;
    std::vector<std::string> discovered;
    int discovered_index = 0;
};

} // namespace

Component MakeModelsTab(AppContext& ctx)
{
    auto st = std::make_shared<ModelsState>();

    auto refresh = [&ctx, st] {
        st->names.clear();
        for (const auto& m : ctx.store.list_models())
            st->names.push_back(m.name);
        st->default_name   = ctx.store.get_default_model_name();
        st->provider_names = ctx.store.list_providers();
        if (st->selected >= static_cast<int>(st->names.size()))
            st->selected = std::max(0, static_cast<int>(st->names.size()) - 1);
        if (st->provider_index >= static_cast<int>(st->provider_names.size()))
            st->provider_index = std::max(0, static_cast<int>(st->provider_names.size()) - 1);
    };
    refresh();

    // -- form open helpers --------------------------------------------------
    auto open_add = [st] {
        st->editing = false;
        st->original_name.clear();
        st->f_name.clear();
        st->f_model_id.clear();
        st->f_max_ctx     = "0";
        st->f_temperature = "0.0";
        st->provider_index = 0;
        st->form_error.clear();
        st->show_form = true;
    };

    auto open_edit = [&ctx, st] {
        if (st->names.empty()) return;
        auto m = ctx.store.get_model(st->names[st->selected]);
        if (!m) return;
        st->editing       = true;
        st->original_name = m->name;
        st->f_name        = m->name;
        st->f_model_id    = m->model_id;
        st->f_max_ctx     = std::to_string(m->max_context_size);
        st->f_temperature = std::to_string(m->temperature);
        st->provider_index = 0;
        for (size_t i = 0; i < st->provider_names.size(); ++i) {
            if (st->provider_names[i] == m->provider) {
                st->provider_index = static_cast<int>(i);
                break;
            }
        }
        st->form_error.clear();
        st->show_form = true;
    };

    // -- form save ----------------------------------------------------------
    auto save = [&ctx, st, refresh] {
        if (st->f_name.empty()) {
            st->form_error = "name must not be empty";
            return;
        }
        if (st->provider_names.empty()) {
            st->form_error = "add a provider first (Providers tab)";
            return;
        }
        int max_ctx = 0;
        if (!ParseInt(st->f_max_ctx, max_ctx) || max_ctx < 0) {
            st->form_error = "max_context_size must be a non-negative integer";
            return;
        }
        double temperature = 0.0;
        if (!ParseDouble(st->f_temperature, temperature)) {
            st->form_error = "temperature must be a number";
            return;
        }
        daedalus::ModelConfig m;
        m.name             = st->f_name;
        m.provider         = st->provider_names[st->provider_index];
        m.model_id         = st->f_model_id;
        m.max_context_size = max_ctx;
        m.temperature      = temperature;

        ctx.store.upsert_model(m);
        if (st->editing && st->original_name != m.name) {
            ctx.store.delete_model(st->original_name);
            if (ctx.store.get_default_model_name() == st->original_name)
                ctx.store.set_default_model_name(m.name);
        }

        refresh();
        ctx.set_status("model '" + m.name + "' saved");
        st->show_form = false;
    };

    // -- default / delete ---------------------------------------------------
    auto set_default = [&ctx, st, refresh] {
        if (st->names.empty()) return;
        ctx.store.set_default_model_name(st->names[st->selected]);
        refresh();
        ctx.set_status("default model set to '" + st->names[st->selected] + "'");
    };

    auto do_delete = [&ctx, st, refresh] {
        if (st->names.empty()) return;
        const std::string name = st->names[st->selected];
        ctx.store.delete_model(name);
        refresh();
        ctx.set_status("model '" + name + "' deleted");
        st->confirm_delete = false;
    };

    // -- discovery ----------------------------------------------------------
    auto discover = [&ctx, st] {
        if (st->provider_names.empty()) {
            st->form_error = "add a provider first (Providers tab)";
            return;
        }
        const auto& pname = st->provider_names[st->provider_index];
        auto p = ctx.store.get_provider(pname);
        if (!p) {
            st->form_error = "provider '" + pname + "' not found";
            return;
        }
        ctx.set_status("discovering models from " + p->host + " …");
        auto result = daedalus::discover_models(p->host, p->api_key, p->discovery_endpoint);
        if (!result.success) {
            st->form_error = "discovery failed: " + result.error_message;
            ctx.set_status("model discovery failed");
            return;
        }
        st->discovered.clear();
        for (const auto& m : result.models)
            st->discovered.push_back(m.id);
        if (st->discovered.empty()) {
            st->form_error = "provider returned no models";
            return;
        }
        st->discovered_index = 0;
        st->show_discover = true;
        ctx.set_status("discovered " + std::to_string(st->discovered.size()) + " models");
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
    auto btn_default = Button(" Set default (s) ", set_default, ButtonOption::Border());
    auto buttons = Container::Horizontal({btn_add, btn_edit, btn_del, btn_default});

    auto body = Container::Vertical({menu, buttons});

    auto body_renderer = Renderer(body, [&ctx, st, menu, btn_add, btn_edit, btn_del, btn_default] {
        Elements detail;
        detail.push_back(text("model details") | bold);
        detail.push_back(separator());
        if (st->names.empty()) {
            detail.push_back(text("(no models — press 'a' to add one)") | dim);
        } else if (auto m = ctx.store.get_model(st->names[st->selected])) {
            const bool is_default = (m->name == st->default_name && !st->default_name.empty());
            detail.push_back(hbox({text("name             : "), text(m->name),
                                   is_default ? text("  (default)") | color(Color::Green) : text("")}));
            detail.push_back(hbox({text("provider         : "), text(m->provider)}));
            detail.push_back(hbox({text("model_id         : "), text(m->model_id)}));
            detail.push_back(hbox({text("max_context_size : "), text(std::to_string(m->max_context_size))}));
            detail.push_back(hbox({text("temperature      : "), text(std::to_string(m->temperature))}));
        }
        return hbox({
            vbox({
                text(" models ") | bold,
                separator(),
                menu->Render() | frame | flex,
            }) | border | size(WIDTH, GREATER_THAN, 28),
            vbox({
                vbox(std::move(detail)) | flex,
                separator(),
                hbox({btn_add->Render(), btn_edit->Render(), btn_del->Render(), btn_default->Render()}),
            }) | border | flex,
        });
    });

    // -- edit form modal ----------------------------------------------------
    auto row_name  = FormRow("name", &st->f_name, false, save);
    auto row_mid   = FormRow("model_id", &st->f_model_id, false, save);
    auto row_ctx   = FormRow("max_context_size", &st->f_max_ctx, false, save);
    auto row_temp  = FormRow("temperature", &st->f_temperature, false, save);
    auto provider_dropdown = Dropdown(&st->provider_names, &st->provider_index);

    auto btn_save     = Button(" Save ", save, ButtonOption::Border());
    auto btn_cancel   = Button(" Cancel ", [st] { st->show_form = false; }, ButtonOption::Border());
    auto btn_discover = Button(" Discover ", discover, ButtonOption::Border());
    auto form = Container::Vertical({
        Container::Vertical({row_name, provider_dropdown, row_mid, row_ctx, row_temp}),
        Container::Horizontal({btn_save, btn_cancel, btn_discover}),
    });

    auto form_modal = Renderer(form,
        [st, row_name, provider_dropdown, row_mid, row_ctx, row_temp, btn_save, btn_cancel, btn_discover] {
            Elements rows;
            rows.push_back(text(st->editing ? " Edit model " : " Add model ") | bold);
            rows.push_back(separator());
            rows.push_back(row_name->Render());
            rows.push_back(hbox({text("provider") | size(WIDTH, EQUAL, 24),
                                 provider_dropdown->Render() | flex}));
            rows.push_back(row_mid->Render());
            rows.push_back(row_ctx->Render());
            rows.push_back(row_temp->Render());
            if (!st->form_error.empty())
                rows.push_back(text(" " + st->form_error) | color(Color::Red));
            rows.push_back(separator());
            rows.push_back(hbox({btn_save->Render(), btn_cancel->Render(), btn_discover->Render()}) | center);
            return vbox(std::move(rows)) | border | size(WIDTH, GREATER_THAN, 60) | clear_under;
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
            text(" Delete model ") | bold,
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

    // -- discovery chooser modal --------------------------------------------
    auto discover_list = Radiobox(&st->discovered, &st->discovered_index);
    auto btn_use = Button(" Use ", [st] {
        if (!st->discovered.empty())
            st->f_model_id = st->discovered[st->discovered_index];
        st->show_discover = false;
    }, ButtonOption::Border());
    auto btn_disc_cancel = Button(" Cancel ", [st] { st->show_discover = false; }, ButtonOption::Border());
    auto discover_buttons = Container::Horizontal({btn_use, btn_disc_cancel});
    auto discover_container = Container::Vertical({discover_list, discover_buttons});
    auto discover_modal = Renderer(discover_container,
        [st, discover_list, btn_use, btn_disc_cancel] {
            return vbox({
                text(" Discovered models ") | bold,
                separator(),
                discover_list->Render() | frame | size(HEIGHT, LESS_THAN, 12),
                separator(),
                hbox({btn_use->Render(), btn_disc_cancel->Render()}) | center,
            }) | border | size(WIDTH, GREATER_THAN, 50) | clear_under | center;
        }) | CatchEvent([st](Event e) {
            if (e == Event::Escape) {
                st->show_discover = false;
                return true;
            }
            return false;
        });

    // -- keyboard shortcuts (only when no modal is open) --------------------
    // Registered on the shared context; the root CatchEvent routes events
    // for the active tab here (see SettingsApp.h).
    ctx.key_handlers.emplace_back([st, open_add, open_edit, set_default](Event e) {
        if (st->show_form || st->confirm_delete || st->show_discover) return false;
        if (e == Event::Character('a')) { open_add(); return true; }
        if (e == Event::Character('e')) { open_edit(); return true; }
        if (e == Event::Character('d')) {
            if (!st->names.empty()) st->confirm_delete = true;
            return true;
        }
        if (e == Event::Character('s')) { set_default(); return true; }
        return false;
    });
    ctx.modal_checks.emplace_back([st] {
        return st->show_form || st->confirm_delete || st->show_discover;
    });

    return body_renderer
         | Modal(form_modal, &st->show_form)
         | Modal(confirm_modal, &st->confirm_delete)
         | Modal(discover_modal, &st->show_discover);
}

} // namespace settings_tui
