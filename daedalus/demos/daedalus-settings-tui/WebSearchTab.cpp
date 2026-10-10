// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-settings-tui/WebSearchTab.cpp

#include "WebSearchTab.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>

namespace settings_tui {

using namespace ftxui;

namespace {

struct ProviderRowState
{
    std::string name;         //!< stable provider id (not editable)
    bool enabled = false;
    std::string api_key;
    std::string base_url;
    std::string username;
    std::string password;
    std::string priority;     //!< edited as text, validated on save
};

struct WebSearchState
{
    std::vector<ProviderRowState> rows;
    std::string form_error;
};


} // namespace

Component MakeWebSearchTab(AppContext& ctx)
{
    auto st = std::make_shared<WebSearchState>();

    ctx.key_handlers.emplace_back([](Event) { return false; });
    ctx.modal_checks.emplace_back([] { return false; });

    auto load = [&ctx, st] {
        st->rows.clear();
        for (const auto& p : ctx.store.get_web_search_providers()) {
            ProviderRowState r;
            r.name     = p.name;
            r.enabled  = p.enabled;
            r.api_key  = p.api_key;
            r.base_url = p.base_url;
            r.username = p.username;
            r.password = p.password;
            r.priority = std::to_string(p.priority);
            st->rows.push_back(std::move(r));
        }
    };

    auto save = [&ctx, st] {
        for (const auto& r : st->rows) {
            int prio = 0;
            if (!ParseInt(r.priority, prio)) {
                st->form_error = r.name + ": priority must be an integer";
                return;
            }
            daedalus::WebSearchProviderSettings p;
            p.name     = r.name;
            p.enabled  = r.enabled;
            p.api_key  = r.api_key;
            p.base_url = r.base_url;
            p.username = r.username;
            p.password = r.password;
            p.priority = prio;
            ctx.store.set_web_search_provider(p);
        }
        st->form_error.clear();
        ctx.set_status("web search settings saved (applies immediately)");
    };

    load();

    // One component tree per provider row.
    std::vector<Component> row_components;
    for (auto& r : st->rows) {
        // Captured by reference: the vector is stable for the component's
        // lifetime (load() runs before the tree exists and Save/Reload do
        // not clear it afterwards).
        auto* row = &r;
        const bool is_searxng = (row->name == "searxng");
        const bool is_ddg     = (row->name == "duckduckgo");

        auto enabled_check = Checkbox(row->name + " enabled", &row->enabled);
        auto row_priority  = FormRow("  priority (lower = tried earlier)",
                                     &row->priority, false, save);

        std::vector<Component> parts{enabled_check, row_priority};
        if (is_searxng) {
            parts.push_back(FormRow("  instance URL", &row->base_url, false, save));
            parts.push_back(FormRow("  API key / bearer token (if token-gated)",
                                    &row->api_key, /*password=*/true, save));
            parts.push_back(FormRow("  basic-auth username (optional)",
                                    &row->username, false, save));
            parts.push_back(FormRow("  basic-auth password (optional)",
                                    &row->password, /*password=*/true, save));
        } else if (!is_ddg) {
            parts.push_back(FormRow("  API key", &row->api_key,
                                    /*password=*/true, save));
        }
        row_components.push_back(Container::Vertical(std::move(parts)));
    }

    auto btn_save   = Button(" Save ", save, ButtonOption::Border());
    auto btn_reload = Button(" Reload ", [load] { load(); }, ButtonOption::Border());

    std::vector<Component> all = row_components;
    all.push_back(Container::Horizontal({btn_save, btn_reload}));
    auto form = Container::Vertical(std::move(all));

    return Renderer(form, [st, row_components, btn_save, btn_reload] {
        Elements blocks;
        blocks.push_back(text(" web search providers (web_search tool) ") | bold);
        blocks.push_back(separator());
        blocks.push_back(
            text(" Enabled providers are tried in priority order; the first "
                 "non-empty result wins.") | dim);
        blocks.push_back(
            text(" A failing or blocked provider falls through to the next. "
                 "duckduckgo needs no key.") | dim);
        for (std::size_t i = 0; i < row_components.size(); ++i) {
            blocks.push_back(separator());
            blocks.push_back(row_components[i]->Render());
            if (st->rows[i].name == "duckduckgo")
                blocks.push_back(text("  key-less — always available") | dim);
        }
        if (!st->form_error.empty())
            blocks.push_back(text(" " + st->form_error) | color(Color::Red));
        blocks.push_back(separator());
        blocks.push_back(hbox({btn_save->Render(), btn_reload->Render()}));
        return vbox(std::move(blocks)) | border | flex;
    });
}

} // namespace settings_tui
