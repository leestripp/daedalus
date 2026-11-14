//! Workspace file tree — the Electron app's workspace-view: the current
//! workspace directory, browsable, dirs expandable, files open via xdg-open.
//! Read directly from disk (the daemon doesn't serve fs listings).

use std::collections::HashSet;
use std::path::{Path, PathBuf};

/// One directory level: (path, is_dir), dirs first, junk filtered.
pub fn children(dir: &Path) -> Vec<(PathBuf, bool)> {
    let mut out = Vec::new();
    if let Ok(entries) = std::fs::read_dir(dir) {
        for entry in entries.flatten() {
            let path = entry.path();
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') || matches!(name.as_str(), "node_modules" | "target" | "build" | "__pycache__") {
                continue;
            }
            let is_dir = path.is_dir();
            out.push((path, is_dir));
        }
    }
    out.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
    out
}

/// Render the tree into a scroll area. Returns nothing; clicks mutate
/// `expanded` (dirs) or open files with the system handler.
pub fn tree_ui(
    ui: &mut egui::Ui,
    root: &Path,
    expanded: &mut HashSet<PathBuf>,
) {
    render_dir(ui, root, 0, expanded);
}

fn render_dir(ui: &mut egui::Ui, dir: &Path, depth: usize, expanded: &mut HashSet<PathBuf>) {
    for (path, is_dir) in children(dir) {
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let indent = "    ".repeat(depth);
        if is_dir {
            let open = expanded.contains(&path);
            let label = format!("{indent}{} {name}", if open { "▾" } else { "▸" });
            if ui.selectable_label(false, label).clicked() {
                if !expanded.insert(path.clone()) {
                    expanded.remove(&path);
                }
            }
            if open {
                render_dir(ui, &path, depth + 1, expanded);
            }
        } else {
            ui.horizontal(|ui| {
                ui.label(format!("{indent}  {name}"));
                if ui.small_button("↗").clicked() {
                    let _ = std::process::Command::new("xdg-open").arg(&path).spawn();
                }
            });
        }
    }
}
