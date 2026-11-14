#include "monkeybyte/ui/dialogs/ModelEditDialog.h"

#include "monkeybyte/ui/UiGuard.h"

#include "monkeybyte/AppServices.h"

#include <daedalus/settings.h>

namespace monkeybyte {

Gtk::Box* ModelEditDialog::make_field(const Glib::ustring& label, Gtk::Widget& field) {
    auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    auto* l = Gtk::make_managed<Gtk::Label>(label);
    l->set_halign(Gtk::Align::START);
    l->add_css_class("dim-label");
    field.set_hexpand(true);
    row->append(*l);
    row->append(field);
    return row;
}

ModelEditDialog::ModelEditDialog(Gtk::Window& parent, AppServices& services,
                                 const std::optional<UiModel>& existing)
    : services_(services) {
    set_title(existing ? "Edit Model" : "Add Model");
    set_transient_for(parent);
    set_modal(true);
    set_default_size(420, -1);

    max_context_.set_range(0, 1048576); // 0 = provider/model default
    max_context_.set_increments(512, 4096);
    temperature_.set_range(0.0, 2.0);
    temperature_.set_increments(0.1, 0.5);
    temperature_.set_digits(2);

    auto* body = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 10);
    body->set_margin(16);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL);
    auto* title = Gtk::make_managed<Gtk::Label>(existing ? "Edit Model" : "Add Model");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);
    auto* close_btn = Gtk::make_managed<Gtk::Button>("×");
    close_btn->set_has_frame(false);
    close_btn->signal_clicked().connect([this] { hide(); });
    header->append(*title);
    header->append(*close_btn);

    // provider dropdown from the shared registry (Settings → Providers)
    auto provider_rows = Gtk::StringList::create({});
    unsigned provider_sel = GTK_INVALID_LIST_POSITION;
    for (const auto& p :
         model_bridge::list_providers(daedalus::SettingsStore::instance())) {
        provider_rows->append(p.name);
        provider_names_.push_back(p.name);
        if (existing && p.name == existing->provider)
            provider_sel = static_cast<unsigned>(provider_names_.size() - 1);
    }
    provider_.set_model(provider_rows);
    if (provider_sel != GTK_INVALID_LIST_POSITION)
        provider_.set_selected(provider_sel);
    provider_.set_sensitive(!existing); // provider is fixed once created

    body->append(*header);
    body->append(*make_field("PROVIDER", provider_));
    body->append(*make_field("NAME (UNIQUE ID)", name_));
    body->append(*make_field("MODEL ID", model_id_));

    auto* split = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
    split->append(*make_field("MAX CONTEXT", max_context_));
    split->append(*make_field("TEMPERATURE", temperature_));
    body->append(*split);

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([this] { hide(); });
    auto* save = Gtk::make_managed<Gtk::Button>("Save");
    save->add_css_class("suggested-action");
    save->signal_clicked().connect([this] { on_save(); });
    footer->append(*cancel);
    footer->append(*save);
    body->append(*footer);

    set_child(*body);

    if (existing) {
        name_.set_text(existing->name);
        name_.set_editable(false); // name is the unique id (§5.3)
        model_id_.set_text(existing->model_id);
        max_context_.set_value(existing->max_context_size);
        temperature_.set_value(existing->temperature);
        is_default_model_ = existing->is_default;
    } else {
        max_context_.set_value(4096);
        temperature_.set_value(0.7);
    }

    auto keys = Gtk::EventControllerKey::create();
    keys->signal_key_pressed().connect(
        [this](guint keyval, guint, Gdk::ModifierType) {
            if (keyval == GDK_KEY_Escape) { hide(); return true; }
            return false;
        },
        false);
    add_controller(keys);
}

void ModelEditDialog::on_save() {
    const unsigned pidx = provider_.get_selected();
    if (name_.get_text().empty() || model_id_.get_text().empty() ||
        pidx == GTK_INVALID_LIST_POSITION || pidx >= provider_names_.size())
        return;
    UiModel m;
    m.name = name_.get_text();
    m.provider = provider_names_[pidx];
    m.model_id = model_id_.get_text();
    m.max_context_size = max_context_.get_value_as_int();
    m.temperature = temperature_.get_value();
    m.is_default = is_default_model_;
    // SQLite write can throw; keep the dialog open on failure.
    ui_guard("ModelEditDialog::on_save", [&] {
        model_bridge::save_model(daedalus::SettingsStore::instance(), m);
        signal_saved_.emit();
        hide();
    });
}

} // namespace monkeybyte
