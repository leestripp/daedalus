#!/usr/bin/env bash
# Install the daedalus monorepo — library, icarus, monkeybyte — into a
# prefix without sudo (default: ~/.local), in ONE pass.
#
#   ./install.sh              # build + install everything to ~/.local
#   ./install.sh /opt/foo     # custom prefix (needs write access)
#
# What lands where (relative to <prefix>):
#   bin/icarus, bin/monkeybyte          the two host apps
#   lib/libdaedalus.a, include/...      the library + headers + CMake package
#   share/monkeybyte/data/              monkeybyte runtime data
#   share/glib-2.0/schemas/             monkeybyte GSettings schema (compiled)
#   share/applications/monkeybyte.desktop, share/icons/...
#
# §Monorepo (2026-09-24): single entry point for both hosts. monkeybyte's
# post-install needs (absolute Exec path in the .desktop file, desktop/icon
# cache refresh) are handled here — see monkeybyte/install.sh, which now
# delegates to this script when run inside the monorepo.
#
# With the default prefix everything is found automatically: GSettings
# searches XDG data dirs (which include ~/.local/share), and ~/.local/bin
# is on most desktop PATHs.
set -euo pipefail
cd "$(dirname "$0")"

prefix="${1:-$HOME/.local}"

# Configure on first run (monkeybyte auto-enables when pkg-config finds the
# GTK stack; otherwise the library + icarus still install).
if [[ ! -f build/CMakeCache.txt ]]; then
    echo "== Configuring superbuild (build/) =="
    cmake -S . -B build
fi

echo "== Building =="
cmake --build build -j"$(nproc)"

echo "== Installing to $prefix =="
cmake --install build --prefix "$prefix"

# --- monkeybyte desktop launcher integration --------------------------------
# (same post-install steps as monkeybyte/install.sh)
desktop_file="$prefix/share/applications/monkeybyte.desktop"
if [[ -f "$desktop_file" ]]; then
    # Absolute Exec path: desktop sessions don't reliably have <prefix>/bin
    # in PATH when resolving .desktop entries.
    sed -i "s|^Exec=.*|Exec=$prefix/bin/monkeybyte|" "$desktop_file"
    # Refresh caches so the entry + icon appear without a relogin (best effort).
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$prefix/share/applications" 2>/dev/null || true
    fi
    if command -v gtk4-update-icon-cache >/dev/null 2>&1; then
        gtk4-update-icon-cache -f -t "$prefix/share/icons/hicolor" 2>/dev/null || true
    elif command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -f -t "$prefix/share/icons/hicolor" 2>/dev/null || true
    fi
fi

echo
echo "Installed to $prefix:"
for app in icarus monkeybyte; do
    [[ -x "$prefix/bin/$app" ]] && echo "  $prefix/bin/$app"
done
if [[ ":$PATH:" != *":$prefix/bin:"* ]]; then
    echo "Note: $prefix/bin is not in your PATH (monkeybyte's desktop entry uses an absolute path, so the launcher still works)."
fi
