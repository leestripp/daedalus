#include "monkeybyte/ui/panels/SearchPanel.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/ui/UiGuard.h"

namespace monkeybyte {

SearchPanel::SearchPanel(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL), services_(services) {
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    header->add_css_class("sidebar-header");
    auto* title = Gtk::make_managed<Gtk::Label>("SEARCH");
    title->set_halign(Gtk::Align::START);
    header->append(*title);
    append(*header);

    auto* entry_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    entry_box->set_margin(8);
    entry_.set_placeholder_text("Search files…");
    entry_.set_hexpand(true);
    entry_box->append(entry_);
    append(*entry_box);

    status_.add_css_class("muted");
    status_.set_vexpand(true);
    scroller_.set_vexpand(true);
    set_status("Type to search files…");
    append(scroller_);

    // 300 ms debounce; Enter searches immediately (§4.3).
    entry_.signal_search_changed().connect([this] {
        debounce_.disconnect();
        debounce_ = Glib::signal_timeout().connect(
            [this] {
                run_search(entry_.get_text());
                return false; // one-shot
            },
            300);
    });
    entry_.signal_activate().connect([this] {
        debounce_.disconnect();
        run_search(entry_.get_text());
    });
}

void SearchPanel::set_status(const Glib::ustring& text) {
    if (text.empty()) {
        scroller_.set_child(results_);
    } else {
        status_.set_text(text);
        scroller_.set_child(status_);
    }
}

void SearchPanel::run_search(const std::string& query) {
    if (query.empty()) {
        set_status("Type to search files…");
        return;
    }
    if (services_.files.workspace().empty()) {
        set_status("No workspace selected");
        return;
    }
    set_status("Searching…");

    std::vector<SearchService::Result> hits;
    ui_guard("run_search", [&] {
        hits = SearchService::search(services_.files.workspace(), query,
                                     services_.store.load_search_excludes());
    });

    while (auto* row = results_.get_row_at_index(0))
        results_.remove(*row);

    if (hits.empty()) {
        set_status("No results found");
        return;
    }
    set_status("");

    auto* count = Gtk::make_managed<Gtk::Label>(
        std::to_string(hits.size()) + (hits.size() == 1 ? " result" : " results"));
    count->set_halign(Gtk::Align::START);
    count->set_margin(6);
    count->add_css_class("dim-label");
    auto* count_row = Gtk::make_managed<Gtk::ListBoxRow>();
    count_row->set_child(*count);
    count_row->set_selectable(false);
    count_row->set_activatable(false);
    results_.append(*count_row);

    for (const auto& hit : hits) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        box->set_margin(6);
        auto* path = Gtk::make_managed<Gtk::Label>(
            hit.path + ":" + std::to_string(hit.line_no));
        path->add_css_class("monospace");
        path->set_halign(Gtk::Align::START);
        path->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
        auto* preview = Gtk::make_managed<Gtk::Label>(hit.preview);
        preview->set_halign(Gtk::Align::START);
        preview->set_ellipsize(Pango::EllipsizeMode::END);
        preview->add_css_class("dim-label");
        box->append(*path);
        box->append(*preview);

        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*box);
        const std::string p = hit.path;
        const int line = hit.line_no;
        auto click = Gtk::GestureClick::create();
        click->signal_pressed().connect(
            [this, p, line](int, double, double) {
                signal_open_file_.emit(p, line); // §4.3: jump to the match
            });
        row->add_controller(click);
        results_.append(*row);
    }
}

} // namespace monkeybyte
