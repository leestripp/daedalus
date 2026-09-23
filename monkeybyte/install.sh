#!/usr/bin/env bash
# Install monkeybyte into a prefix without sudo (default: ~/.local).
#
#   ./install.sh              # installs to ~/.local
#   ./install.sh /opt/foo     # installs to a custom prefix (needs write access)
#
# The GSettings schema is compiled into <prefix>/share/glib-2.0/schemas;
# with the default prefix GSettings finds it automatically via XDG data dirs.
#
# Host notes (AerynOS atomic): everything lands under the user prefix …â€”…
# /usr is managed by moss and stays untouched. The installed .desktop file
# is rewritten to an absolute Exec path so the app launcher works even when
# the desktop session's PATH does not include <prefix>/bin, and the
# desktop/icon caches are refreshed so the entry shows up immediately.
set -euo pipefail
cd "$(dirname "$0")"

prefix="${1:-$HOME/.local}"

cmake --build build
cmake --install build --prefix "$prefix"

# --- desktop launcher integration -----------------------------------------
desktop_file="$prefix/share/applications/monkeybyte.desktop"
if [[ -f "$desktop_file" ]]; then
    # Absolute Exec path: atomic/desktop sessions don't reliably have
    # <prefix>/bin in PATH when resolving .desktop entries.
    sed -i "s|^Exec=.*|Exec=$prefix/bin/monkeybyte|" "$desktop_file"
fi
# Refresh caches so the entry + icon appear without a relogin (best effort).
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$prefix/share/applications" 2>/dev/null || true
fi
if command -v gtk4-update-icon-cache >/dev/null 2>&1; then
    gtk4-update-icon-cache -f -t "$prefix/share/icons/hicolor" 2>/dev/null || true
elif command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$prefix/share/icons/hicolor" 2>/dev/null || true
fi

echo "Installed to $prefix"
echo "Run with: $prefix/bin/monkeybyte"
if [[ ":$PATH:" != *":$prefix/bin:"* ]]; then
    echo "Note: $prefix/bin is not in your PATH (the desktop entry uses an absolute path, so the launcher still works)."
fi
