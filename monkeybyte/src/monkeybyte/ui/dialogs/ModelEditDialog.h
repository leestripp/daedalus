#pragma once

#include "monkeybyte/backend/ModelBridge.h"

#include <gtkmm.h>
#include <optional>
#include <sigc++/sigc++.h>
#include <vector>

namespace monkeybyte {

struct AppServices;

// §5.3 — create or edit a model. Endpoint/credential config comes from the
// provider picked in the dropdown (Settings → Providers), not re-entered here.
class ModelEditDialog : public Gtk::Window {
public:
    // existing == nullopt → create mode.
    ModelEditDialog(Gtk::Window& parent, AppServices& services,
                    const std::optional<UiModel>& existing = std::nullopt);

    sigc::signal<void()> signal_saved() { return signal_saved_; }

private:
    void on_save();
    Gtk::Box* make_field(const Glib::ustring& label, Gtk::Widget& field);

    AppServices& services_;
    bool is_default_model_ = false;
    std::vector<std::string> provider_names_; // dropdown rows, by index

    Gtk::Entry name_, model_id_;
    Gtk::DropDown provider_;
    Gtk::SpinButton max_context_, temperature_;

    sigc::signal<void()> signal_saved_;
};

} // namespace monkeybyte
