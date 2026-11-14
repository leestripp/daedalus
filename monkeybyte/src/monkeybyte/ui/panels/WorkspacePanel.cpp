#include "monkeybyte/ui/panels/WorkspacePanel.h"

#include "monkeybyte/AppServices.h"
#include "monkeybyte/backend/FountainTools.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/backend/ProjectTypes.h"
#include "monkeybyte/ui/dialogs/NewFileDialog.h"
#include "monkeybyte/ui/dialogs/NewFromTemplateDialog.h"
#include "monkeybyte/ui/dialogs/NewFolderDialog.h"
#include "monkeybyte/ui/dialogs/NewWorkspaceDialog.h"
#include "monkeybyte/ui/dialogs/RenameDialog.h"
#include "monkeybyte/ui/dialogs/WorkspacePickerDialog.h"
#include "monkeybyte/ui/UiGuard.h"

#include <cstdlib>
#include <optional>
#include <filesystem>
#include <system_error>

namespace monkeybyte {

namespace {

Gtk::Window* toplevel_of(Gtk::Widget& w) {
    return dynamic_cast<Gtk::Window*>(w.get_root());
}

} // namespace

WorkspacePanel::WorkspacePanel(AppServices& services)
    : Gtk::Box(Gtk::Orientation::VERTICAL), services_(services) {
    // ── header: WORKSPACE + 📁 ↻ + (§4.2) ────────────────────────────
    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    header->add_css_class("sidebar-header");
    auto* title = Gtk::make_managed<Gtk::Label>("WORKSPACE");
    title->set_halign(Gtk::Align::START);
    title->set_hexpand(true);

    auto icon_btn = [](const Glib::ustring& icon, const Glib::ustring& tip) {
        auto* b = Gtk::make_managed<Gtk::Button>(icon);
        b->set_has_frame(false);
        b->set_size_request(24, 24);
        b->set_tooltip_text(tip);
        return b;
    };
    auto* open_btn = icon_btn("📁", "Open workspace folder");
    auto* refresh_btn = icon_btn("↻", "Refresh");

    // '+' popup menu — same Gio::Menu + SimpleActionGroup pattern as
    // EditorArea's tab-bar dropdown (§4.2). Sections: create / open / view.
    auto* new_btn = Gtk::make_managed<Gtk::MenuButton>();
    new_btn->set_label("+");
    new_btn->set_has_frame(false);
    new_btn->set_size_request(24, 24);
    new_btn->set_always_show_arrow(false);
    new_btn->set_tooltip_text("New file or folder");
    auto menu = Gio::Menu::create();
    auto sec_workspace = Gio::Menu::create();
    sec_workspace->append("New Workspace…", "workspace_header.new_workspace");
    menu->append_section(sec_workspace);
    auto sec_create = Gio::Menu::create();
    sec_create->append("Add File", "workspace_header.new_file");
    sec_create->append("Add Folder", "workspace_header.new_folder");
    sec_create->append("Add File from Template…",
                       "workspace_header.new_from_template");
    sec_create->append("Import File(s)…", "workspace_header.import_files");
    menu->append_section(sec_create);
    auto sec_type = Gio::Menu::create();
    sec_type->append("Set Workspace Type…", "workspace_header.set_type");
    menu->append_section(sec_type);
    auto sec_open = Gio::Menu::create();
    sec_open->append("Reveal in File Manager",
                     "workspace_header.reveal_in_file_manager");
    sec_open->append("Open Terminal Here", "workspace_header.open_terminal");
    menu->append_section(sec_open);
    auto sec_view = Gio::Menu::create();
    sec_view->append("Show Hidden Files", "workspace_header.toggle_hidden");
    menu->append_section(sec_view);

    auto group = Gio::SimpleActionGroup::create();
    group->add_action("new_file", [this] { new_file(); });
    group->add_action("new_folder", [this] {
        const auto& ws = services_.files.workspace();
        if (ws.empty()) {
            Logger::instance().error("Add Folder: no workspace selected");
            return;
        }
        new_folder(ws.string());
    });
    group->add_action("new_workspace", [this] {
        auto* toplevel = toplevel_of(*this);
        auto* dlg = Gtk::make_managed<NewWorkspaceDialog>(
            toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>(),
            services_);
        // Created + scaffolded folder opens like a picked workspace.
        dlg->signal_workspace_created().connect(
            [this](const std::string& path) {
                signal_workspace_changed_.emit(path);
            });
        dlg->present();
    });
    group->add_action("new_from_template",
                      [this] { new_file_from_template(); });
    group->add_action("import_files", [this] { import_files(); });
    group->add_action("set_type", [this] { set_workspace_type(); });
    group->add_action("reveal_in_file_manager",
                      [this] { reveal_in_file_manager(); });
    group->add_action("open_terminal", [this] { open_terminal_here(); });

    // "Show Hidden Files": stateful boolean → menu draws a checkmark.
    // The settings store is authoritative so the checkmark stays in sync
    // with Settings → Explorer even if it changed since this panel was built.
    // Guarded load: a SQLite hiccup must not clobber every other interface
    // setting with defaults (same pattern as SettingsPanel's guarded save)
    // nor throw out of a GTK callback.
    auto guarded_show_hidden = [this]() -> std::optional<bool> {
        std::optional<bool> v;
        ui_guard("workspace load_interface",
                 [&] { v = services_.store.load_interface().show_hidden; });
        return v;
    };
    auto hidden_action = Gio::SimpleAction::create_bool(
        "toggle_hidden", guarded_show_hidden().value_or(false));
    hidden_action->signal_activate().connect(
        [this, hidden_action, guarded_show_hidden](const Glib::VariantBase&) {
            const auto cur = guarded_show_hidden();
            if (!cur) return; // DB hiccup: leave state and store untouched
            const bool next = !*cur;
            bool saved = false;
            ui_guard("workspace save_interface", [&] {
                auto s = services_.store.load_interface();
                s.show_hidden = next;
                services_.store.save_interface(s);
                saved = true;
            });
            if (!saved) return;
            services_.files.set_show_hidden(next); // live apply
            tree_.refresh();
            hidden_action->change_state(next);
        });
    group->add_action(hidden_action);

    new_btn->insert_action_group("workspace_header", group);
    new_btn->set_menu_model(menu);

    open_btn->signal_clicked().connect([this] { show_picker(); });
    refresh_btn->signal_clicked().connect([this] { tree_.refresh(); });

    header->append(*title);
    header->append(*open_btn);
    header->append(*refresh_btn);
    header->append(*new_btn);
    append(*header);

    // ── workspace path bar (clickable → picker) ──────────────────────
    path_bar_.add_css_class("workspace-path-bar");
    path_bar_.set_halign(Gtk::Align::START);
    path_bar_.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
    path_bar_.set_tooltip_text("Click to change workspace");
    auto path_click = Gtk::GestureClick::create();
    path_click->set_button(GDK_BUTTON_PRIMARY);
    path_click->signal_pressed().connect([this](int, double, double) { show_picker(); });
    path_bar_.add_controller(path_click);
    append(path_bar_);

    // ── tree ─────────────────────────────────────────────────────────
    tree_.set_vexpand(true);
    tree_.signal_open_file().connect(
        [this](const std::string& p, int line) { signal_open_file_.emit(p, line); });
    tree_.signal_open_file_as().connect(
        [this](const std::string& p, FileOpenMode mode) {
            signal_open_file_as_.emit(p, mode);
        });
    tree_.signal_show_picker().connect([this] { show_picker(); });
    tree_.signal_new_file_in().connect(
        [this](const std::string& dir) { new_file(dir); });
    tree_.signal_new_folder_in().connect(
        [this](const std::string& dir) { new_folder(dir); });
    tree_.signal_rename_requested().connect(
        [this](const std::string& p, bool is_dir) { rename_entry(p, is_dir); });
    tree_.signal_delete_requested().connect(
        [this](const std::string& p, bool is_dir) { delete_entry(p, is_dir); });
    tree_.signal_export_pdf_requested().connect(
        [this](const std::string& p) { export_fountain_pdf(p); });
    append(tree_);

    refresh();
}

void WorkspacePanel::refresh() {
    const auto& ws = services_.files.workspace();
    path_bar_.set_text(ws.empty() ? "No workspace selected" : ws.string());
    tree_.set_workspace(&services_.files);
}


void WorkspacePanel::reveal(const std::string& path) {
    tree_.reveal_path(path);
}

void WorkspacePanel::set_workspace_type() {
    const auto& ws = services_.files.workspace();
    if (ws.empty()) {
        Logger::instance().error("Set Workspace Type: no workspace selected");
        return;
    }
    const auto& types = services_.project_types.list();
    if (types.empty()) return;

    auto* dlg = Gtk::make_managed<Gtk::Window>();
    dlg->set_title("Set Workspace Type");
    dlg->set_transient_for(*toplevel_of(*this));
    dlg->set_modal(true);
    dlg->set_default_size(360, -1);

    auto* v = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    v->set_margin(16);
    auto* blurb = Gtk::make_managed<Gtk::Label>(
        "Focuses the agent for this workspace (system prompt, templates). "
        "Stored as a stamp in AGENTS.md.");
    blurb->set_wrap(true);
    blurb->set_halign(Gtk::Align::START);
    blurb->add_css_class("dim-label");
    v->append(*blurb);

    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->set_selection_mode(Gtk::SelectionMode::SINGLE);
    list->add_css_class("navigation-sidebar");
    const auto* active = services_.project_types.detect(ws.string());
    for (const auto& t : types) {
        auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
        row_box->set_margin(4);
        auto* name = Gtk::make_managed<Gtk::Label>(t.name);
        name->set_halign(Gtk::Align::START);
        row_box->append(*name);
        if (!t.description.empty()) {
            auto* d = Gtk::make_managed<Gtk::Label>(t.description);
            d->set_halign(Gtk::Align::START);
            d->add_css_class("dim-label");
            row_box->append(*d);
        }
        list->append(*row_box);
    }
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_child(*list);
    scroller->set_size_request(-1, 200);
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    v->append(*scroller);

    auto* status = Gtk::make_managed<Gtk::Label>();
    status->add_css_class("error-label");
    status->set_halign(Gtk::Align::START);
    v->append(*status);

    auto* footer = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    footer->set_halign(Gtk::Align::END);
    auto* cancel = Gtk::make_managed<Gtk::Button>("Cancel");
    cancel->signal_clicked().connect([dlg] { dlg->hide(); });
    auto* apply = Gtk::make_managed<Gtk::Button>("Apply");
    apply->add_css_class("suggested-action");
    apply->signal_clicked().connect([this, dlg, list, &types, ws, status] {
        auto* row = list->get_selected_row();
        if (!row) { status->set_text("Pick a type first."); return; }
        const auto& t = types.at(row->get_index());
        std::string err;
        if (!ProjectTypeLibrary::write_stamp(ws.string(), t.id, &err)) {
            status->set_text(err);
            return;
        }
        dlg->hide();
        signal_project_type_changed_.emit();
    });
    footer->append(*cancel);
    footer->append(*apply);
    v->append(*footer);

    // preselect the current type
    for (size_t i = 0; i < types.size(); ++i)
        if (active && types[i].id == active->id) {
            if (auto* row = list->get_row_at_index(static_cast<int>(i)))
                list->select_row(*row);
            break;
        }

    dlg->set_child(*v);
    dlg->present();
}

void WorkspacePanel::export_fountain_pdf(const std::string& path) {
    const std::string pdf =
        std::filesystem::path(path).replace_extension(".pdf").string();
    std::string err;
    const bool ok = fountain::to_pdf(path, pdf, &err);
    auto* toplevel = toplevel_of(*this);
    auto dlg = Gtk::AlertDialog::create( // shared_ptr keeps it alive
        ok ? "PDF exported" : "PDF export failed");
    dlg->set_detail(ok ? pdf : err);
    if (ok) tree_.refresh(); // surface the new file in the tree
    dlg->show(toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>());
}

void WorkspacePanel::show_picker() {
    auto* toplevel = dynamic_cast<Gtk::Window*>(get_root());
    auto* dlg = Gtk::make_managed<WorkspacePickerDialog>(
        toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>(), services_);
    dlg->signal_folder_chosen().connect(
        [this](const std::string& path) { signal_workspace_changed_.emit(path); });
    dlg->present();
}

void WorkspacePanel::new_file(const std::string& initial_dir) {
    auto* toplevel = dynamic_cast<Gtk::Window*>(get_root());
    auto* dlg = Gtk::make_managed<NewFileDialog>(
        toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>(), services_,
        initial_dir);
    dlg->signal_file_created().connect([this](const std::string& path) {
        tree_.refresh(); // §4.2: refresh after create
        signal_open_file_.emit(path, 0);
    });
    dlg->present();
}

void WorkspacePanel::new_folder(const std::string& dir) {
    auto* toplevel = toplevel_of(*this);
    // Rooted at the workspace; `dir` pre-selects but the user can retarget
    // via the in-dialog folder selector (§4.2).
    auto* dlg = Gtk::make_managed<NewFolderDialog>(
        toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>(),
        services_.files.workspace().string(), dir);
    dlg->signal_folder_named().connect(
        [this](const std::string& parent_dir, const std::string& name) {
            const auto target =
                (std::filesystem::path(parent_dir) / name).string();
            if (auto r = services_.files.mkdir(target))
                tree_.refresh(); // §4.2: refresh after create
            else
                Logger::instance().error("mkdir failed: " + r.error());
        });
    dlg->present();
}

void WorkspacePanel::new_file_from_template() {
    if (services_.files.workspace().empty()) {
        Logger::instance().error("Add File from Template: no workspace selected");
        return;
    }
    auto* toplevel = toplevel_of(*this);
    auto* dlg = Gtk::make_managed<NewFromTemplateDialog>(
        toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>(), services_);
    dlg->signal_file_created().connect([this](const std::string& path) {
        tree_.refresh(); // §4.2: refresh after create
        signal_open_file_.emit(path, 0);
    });
    dlg->present();
}

void WorkspacePanel::import_files() {
    const auto root = services_.files.workspace();
    if (root.empty()) {
        Logger::instance().error("Import File(s): no workspace selected");
        return;
    }
    auto* toplevel = toplevel_of(*this);
    if (!toplevel) return;
    auto dialog = Gtk::FileDialog::create();
    dialog->set_title("Import file(s) into workspace");
    dialog->open_multiple(
        *toplevel, [this, dialog, root](Glib::RefPtr<Gio::AsyncResult>& result) {
            std::vector<Glib::RefPtr<Gio::File>> files;
            try {
                files = dialog->open_multiple_finish(result);
            } catch (const Gtk::DialogError&) {
                return; // cancelled
            } catch (const Glib::Error& e) {
                Logger::instance().error(std::string("import failed: ") + e.what());
                return;
            }
            int imported = 0;
            for (const auto& file : files) {
                if (!file || file->get_path().empty()) continue;
                const auto dst = root / std::filesystem::path(file->get_path()).filename();
                std::error_code ec;
                std::filesystem::copy_file(file->get_path(), dst,
                                           std::filesystem::copy_options::none, ec);
                if (ec)
                    Logger::instance().error("import: " + file->get_path() + ": " +
                                             ec.message());
                else
                    ++imported;
            }
            if (imported > 0) tree_.refresh(); // §4.2: refresh after create
        });
}

void WorkspacePanel::reveal_in_file_manager() {
    const auto& ws = services_.files.workspace();
    if (ws.empty()) {
        Logger::instance().error("Reveal in File Manager: no workspace selected");
        return;
    }
    auto* toplevel = toplevel_of(*this);
    if (!toplevel) return;
    auto launcher =
        Gtk::FileLauncher::create(Gio::File::create_for_path(ws.string()));
    launcher->launch(*toplevel,
                     [launcher](Glib::RefPtr<Gio::AsyncResult>& result) {
                         try {
                             launcher->launch_finish(result);
                         } catch (const Glib::Error& e) {
                             Logger::instance().error(
                                 std::string("reveal failed: ") + e.what());
                         }
                     });
}

void WorkspacePanel::open_terminal_here() {
    const auto& ws = services_.files.workspace();
    if (ws.empty()) {
        Logger::instance().error("Open Terminal Here: no workspace selected");
        return;
    }
    // $TERMINAL first, then the usual suspects (first found on PATH wins).
    std::string term;
    if (const char* env = std::getenv("TERMINAL");
        env && *env && !Glib::find_program_in_path(env).empty())
        term = env;
    else
        for (const char* t : {"kgx", "gnome-terminal", "konsole",
                              "xfce4-terminal", "alacritty", "kitty", "xterm"})
            if (!Glib::find_program_in_path(t).empty()) {
                term = t;
                break;
            }
    if (term.empty()) {
        Logger::instance().error("Open Terminal Here: no terminal emulator found");
        return;
    }
    // Terminals inherit the spawner's cwd, so `cd` in a shell works for all
    // of them — no per-terminal --working-directory flag matrix.
    std::string quoted = "'";
    for (const char c : ws.string())
        quoted += (c == '\'') ? "'\\''" : std::string(1, c);
    quoted += "'";
    try {
        Gio::Subprocess::create({"/bin/sh", "-c", "cd " + quoted + " && exec " + term},
                                Gio::Subprocess::Flags::NONE);
    } catch (const Glib::Error& e) {
        Logger::instance().error(std::string("terminal launch failed: ") + e.what());
    }
}

void WorkspacePanel::rename_entry(const std::string& path, bool is_dir) {
    auto* toplevel = toplevel_of(*this);
    const std::string old_name = std::filesystem::path(path).filename().string();
    auto* dlg = Gtk::make_managed<RenameDialog>(
        toplevel ? *toplevel : *Gtk::make_managed<Gtk::Window>(), old_name, is_dir);
    dlg->signal_renamed().connect([this, path](const std::string& new_name) {
        const auto parent = std::filesystem::path(path).parent_path();
        const auto new_path = (parent / new_name).string();
        if (new_path == path) return;
        auto result = services_.files.rename(path, new_path);
        if (!result) {
            Logger::instance().error("rename failed: " + result.error());
            return;
        }
        tree_.refresh();
        signal_path_renamed_.emit(path, new_path);
    });
    dlg->present();
}

void WorkspacePanel::delete_entry(const std::string& path, bool is_dir) {
    const std::string name = std::filesystem::path(path).filename().string();
    auto* dialog = gtk_alert_dialog_new("Delete %s \"%s\"?",
                                        is_dir ? "folder" : "file", name.c_str());
    gtk_alert_dialog_set_detail(
        GTK_ALERT_DIALOG(dialog),
        is_dir ? "The folder and everything inside it will be permanently deleted."
               : "This file will be permanently deleted.");
    const char* buttons[] = {"Cancel", "Delete", nullptr};
    gtk_alert_dialog_set_buttons(GTK_ALERT_DIALOG(dialog), buttons);
    gtk_alert_dialog_set_cancel_button(GTK_ALERT_DIALOG(dialog), 0);
    gtk_alert_dialog_set_default_button(GTK_ALERT_DIALOG(dialog), 0);

    struct Ctx {
        WorkspacePanel* self;
        std::string path;
    };
    auto* ctx = new Ctx{this, path};
    gtk_alert_dialog_choose(
        GTK_ALERT_DIALOG(dialog),
        GTK_WINDOW(toplevel_of(*this) ? toplevel_of(*this)->gobj() : nullptr),
        nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            std::unique_ptr<Ctx> ctx(static_cast<Ctx*>(data));
            const int choice = gtk_alert_dialog_choose_finish(
                GTK_ALERT_DIALOG(source), result, nullptr);
            if (choice == 1) {
                if (auto r = ctx->self->services_.files.remove(ctx->path)) {
                    ctx->self->tree_.refresh(); // §4.2: refresh after delete
                    ctx->self->signal_path_deleted_.emit(ctx->path);
                } else {
                    Logger::instance().error("delete failed: " + r.error());
                }
            }
        },
        ctx);
    g_object_unref(dialog);
}

} // namespace monkeybyte
