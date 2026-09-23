#pragma once

#include "monkeybyte/backend/SearchService.h"

#include <gtkmm.h>
#include <sigc++/sigc++.h>

namespace monkeybyte {

struct AppServices;

// §4.3 — debounced content search with clickable results.
class SearchPanel : public Gtk::Box {
public:
    explicit SearchPanel(AppServices& services);

    sigc::signal<void(const std::string&, int)> signal_open_file() {
        return signal_open_file_;
    }

private:
    void run_search(const std::string& query);
    void set_status(const Glib::ustring& text);

    AppServices& services_;
    Gtk::SearchEntry entry_;
    Gtk::ScrolledWindow scroller_;
    Gtk::ListBox results_;
    Gtk::Label status_;
    sigc::connection debounce_;

    sigc::signal<void(const std::string&, int)> signal_open_file_;
};

} // namespace monkeybyte
