#include "monkeybyte/tree/WorkspaceTree.h"

#include "monkeybyte/backend/FileKind.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/ui/FileIcons.h"
#include "monkeybyte/ui/UiGuard.h"

#include <cctype>
#include <functional>

namespace monkeybyte {

WorkspaceTree::WorkspaceTree() {
    status_.add_css_class("muted");
    set_status("No workspace selected");
    set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
}

bool WorkspaceTree::is_expanded(const std::unordered_set<std::string>& set,
                                const std::string& path) {
    return set.find(path) != set.end();
}

void WorkspaceTree::set_workspace(FileService* files) {
    files_ = files;
    expanded_paths_.clear(); // §4.2: setWorkspace clears expansion memory
    selected_path_.clear();
    refresh();
}

void WorkspaceTree::set_status(const Glib::ustring& text) {
    if (text.empty()) {
        set_child(list_);
    } else {
        status_.set_text(text);
        set_child(status_);
    }
}

void WorkspaceTree::refresh() {
    if (!files_ || files_->workspace().empty()) {
        set_status("No workspace selected");
        signal_refreshed_.emit();
        return;
    }
    rebuild_rows();
    signal_refreshed_.emit();
}

void WorkspaceTree::rebuild_rows() {
    while (auto* row = list_.get_row_at_index(0))
        list_.remove(*row);

    auto root = files_->list("");
    if (!root) {
        set_status("Error loading workspace: " + root.error());
        return;
    }
    if (root->empty()) {
        set_status("Workspace is empty");
        return;
    }
    set_status("");

    // Depth-first render; descend only into expanded folders (§4.2 lazy).
    std::function<void(const std::string&, int)> render =
        [&](const std::string& dir, int depth) {
            auto entries = dir.empty() ? files_->list("") : files_->list(dir);
            if (!entries) return;
            for (const auto& e : *entries) {
                const bool expanded =
                    e.is_dir && is_expanded(expanded_paths_, e.path);

                auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
                box->set_margin_start(4 + depth * 16);
                box->set_margin_top(2);
                box->set_margin_bottom(2);

                auto* arrow = Gtk::make_managed<Gtk::Label>(expanded ? "▾" : "▸");
                arrow->set_visible(e.is_dir); // files: hidden arrow (§4.2)
                auto* icon = Gtk::make_managed<Gtk::Image>();
                icon->set_from_icon_name(file_icon_name(e.name, e.is_dir, expanded));
                icon->set_pixel_size(16);
                auto* name = Gtk::make_managed<Gtk::Label>(e.name);
                name->set_halign(Gtk::Align::START);
                name->set_hexpand(true);
                name->set_ellipsize(Pango::EllipsizeMode::END);

                box->append(*arrow);
                box->append(*icon);
                box->append(*name);

                auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
                row->set_child(*box);
                row->set_tooltip_text(e.path);
                row->set_activatable(true);
                row->set_selectable(true);
                if (e.path == selected_path_)
                    row->add_css_class("active");

                const std::string path = e.path;
                const bool is_dir = e.is_dir;
                auto click = Gtk::GestureClick::create();
                click->set_button(GDK_BUTTON_PRIMARY);
                click->signal_pressed().connect([this, path, is_dir](int, double, double) {
                    // The open-file slots do file I/O + markdown/WebKit work
                    // — a throw must not escape the gesture callback.
                    ui_guard("tree row pressed", [&] {
                        if (is_dir) {
                            toggle_folder(path);
                        } else {
                            selected_path_ = path;
                            open_entry(path);
                            rebuild_rows(); // refresh selection highlight
                        }
                    });
                });
                row->add_controller(click);

                // Right-click context menu (§4.2)
                auto menu_click = Gtk::GestureClick::create();
                menu_click->set_button(GDK_BUTTON_SECONDARY);
                menu_click->signal_pressed().connect(
                    [this, row, path, is_dir](int, double x, double y) {
                        show_context_menu(*row, path, is_dir, x, y);
                    });
                row->add_controller(menu_click);

                list_.append(*row);
                if (expanded) render(e.path, depth + 1);
            }
        };

    render("", 0);
}

// Text-like files open in the editor; media and other binary formats are
// handed to the system-default application (freedesktop associations).
void WorkspaceTree::open_entry(const std::string& path) {
    if (is_text_like_file(path))
        signal_open_file_.emit(path, 0);
    else
        open_external(path);
}

void WorkspaceTree::open_external(const std::string& path) {
    std::string error;
    if (!launch_external(path, error))
        Logger::instance().error("Open with System Application: " + error);
}

void WorkspaceTree::toggle_folder(const std::string& path) {
    if (is_expanded(expanded_paths_, path))
        expanded_paths_.erase(path);
    else
        expanded_paths_.insert(path);
    rebuild_rows(); // refresh preserves expansion set (§4.2)
}

void WorkspaceTree::select(const std::string& path) {
    selected_path_ = path;
    rebuild_rows();
}

void WorkspaceTree::refresh_folder(const std::string& path) {
    // Local FS: a full lazy rebuild is equivalent and cheap (§4.2).
    (void)path;
    rebuild_rows();
}

// Right-click menu (§4.2). Actions are emitted as signals; the owning panel
// performs the FileService work. Copy Path is handled here directly.
void WorkspaceTree::show_context_menu(Gtk::ListBoxRow& row,
                                      const std::string& path, bool is_dir,
                                      double x, double y) {
    auto menu = Gio::Menu::create();
    auto group = Gio::SimpleActionGroup::create();

    const std::string dir = is_dir
        ? path
        : std::filesystem::path(path).parent_path().string();

    // Actions run when the GAction fires through GTK; the panel slots do
    // FileService work that must not throw back into GTK.
    auto guarded = [](auto fn) {
        return [fn = std::move(fn)]() mutable {
            ui_guard("tree context action", fn);
        };
    };

    if (!is_dir) {
        menu->append("Open", "ctx.open");
        group->add_action("open",
                          guarded([this, path] { open_entry(path); }));
        // Always offer the system-default app — for binary/media files it is
        // the primary viewer; for text files it is an escape hatch to the
        // user's preferred external editor/tool.
        menu->append("Open with System Application", "ctx.open_external");
        group->add_action("open_external",
                          guarded([this, path] { open_external(path); }));

        std::string ext = std::filesystem::path(path).extension().string();
        if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
        for (auto& c : ext) c = static_cast<char>(std::tolower(c));
        if (ext == "md" || ext == "markdown") {
            menu->append("Open as Markdown Preview", "ctx.open_md_preview");
            group->add_action("open_md_preview", guarded([this, path] {
                signal_open_file_as_.emit(path, FileOpenMode::MarkdownPreview);
            }));
        } else if (ext == "html" || ext == "htm") {
            menu->append("Open as HTML Preview", "ctx.open_html_preview");
            group->add_action("open_html_preview", guarded([this, path] {
                signal_open_file_as_.emit(path, FileOpenMode::HtmlPreview);
            }));
        } else if (ext == "fountain") {
            // Plan.md P4 — screenplay preview/PDF via an external Fountain
            // converter (wrap/screenplain); degrade gracefully when absent.
            menu->append("Open as Screenplay Preview", "ctx.open_fountain_preview");
            group->add_action("open_fountain_preview", guarded([this, path] {
                signal_open_file_as_.emit(path, FileOpenMode::FountainPreview);
            }));
            menu->append("Export to PDF…", "ctx.export_fountain_pdf");
            group->add_action("export_fountain_pdf", guarded([this, path] {
                signal_export_pdf_requested_.emit(path);
            }));
        }
    }
    menu->append("New File Here", "ctx.new_file");
    group->add_action("new_file",
                      guarded([this, dir] { signal_new_file_in_.emit(dir); }));
    if (is_dir) {
        menu->append("New Folder Here", "ctx.new_folder");
        group->add_action("new_folder",
                          guarded([this, dir] { signal_new_folder_in_.emit(dir); }));
    }
    menu->append("Rename…", "ctx.rename");
    group->add_action("rename", guarded([this, path, is_dir] {
        signal_rename_requested_.emit(path, is_dir);
    }));
    menu->append("Delete…", "ctx.delete");
    group->add_action("delete", guarded([this, path, is_dir] {
        signal_delete_requested_.emit(path, is_dir);
    }));
    menu->append("Copy Path", "ctx.copy_path");
    group->add_action("copy_path", guarded([&row, path] {
        row.get_clipboard()->set_text(path);
    }));

    row.insert_action_group("ctx", group);
    auto* popover = Gtk::make_managed<Gtk::PopoverMenu>(menu);
    popover->set_parent(row);
    popover->set_has_arrow(false);
    popover->set_pointing_to(Gdk::Rectangle(
        static_cast<int>(x), static_cast<int>(y), 1, 1));
    popover->popup();
}

void WorkspaceTree::reveal_path(const std::string& path) {
    // Expand every ancestor folder, then select.
    std::string p = path;
    const std::string ws = files_ ? files_->workspace().string() : "";
    while (p.size() > ws.size() + 1) {
        const auto pos = p.rfind('/');
        if (pos == std::string::npos) break;
        p.resize(pos);
        if (p.size() >= ws.size()) expanded_paths_.insert(p);
    }
    select(path);
}

} // namespace monkeybyte
