#!/usr/bin/env bash
# Install the daedalus monorepo — library, Icarus, and Electron MonkeyByte —
# into a prefix without sudo (default: ~/.local), in ONE pass.
#
#   ./install.sh              # build + install everything to ~/.local
#   ./install.sh /opt/foo     # custom prefix (needs write access)
set -euo pipefail
cd "$(dirname "$0")"

prefix="${1:-$HOME/.local}"

if [[ ! -f build/CMakeCache.txt ]]; then
    echo "== Configuring superbuild (build/) =="
    cmake -S . -B build
fi

echo "== Building =="
cmake --build build -j"$(nproc)"

echo "== Installing Daedalus and Icarus to $prefix =="
cmake --install build --prefix "$prefix"

echo "== Packaging MonkeyByte =="
node monkeybyte/scripts/package.js --skip-build

electron_package="monkeybyte/release/MonkeyByte-linux-x64"
electron_prefix="$prefix/lib/monkeybyte-electron"
rm -rf "$electron_prefix"
mkdir -p "$electron_prefix" "$prefix/bin"
cp -a "$electron_package"/. "$electron_prefix"/
ln -sfn ../lib/monkeybyte-electron/monkeybyte "$prefix/bin/monkeybyte-electron"
# Retire the old GTK executable. Electron keeps its explicit command name.
rm -f "$prefix/bin/monkeybyte"

applications="$prefix/share/applications"
icons="$prefix/share/icons/hicolor/scalable/apps"
mkdir -p "$applications" "$icons"
cp "$electron_package/monkeybyte.desktop" "$applications/monkeybyte.desktop"
cp "$electron_package/monkeybyte.svg" "$icons/monkeybyte.svg"
sed -i "s|^Exec=.*|Exec=$prefix/bin/monkeybyte-electron %U|g" "$applications/monkeybyte.desktop"
# There is one launcher, named MonkeyByte; do not leave a duplicate entry.
rm -f "$applications/monkeybyte-electron.desktop"
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$applications" 2>/dev/null || true
fi
if command -v gtk4-update-icon-cache >/dev/null 2>&1; then
    gtk4-update-icon-cache -f -t "$prefix/share/icons/hicolor" 2>/dev/null || true
elif command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$prefix/share/icons/hicolor" 2>/dev/null || true
fi

echo
echo "Installed to $prefix:"
for app in icarus monkeybyte-electron; do
    [[ -x "$prefix/bin/$app" ]] && echo "  $prefix/bin/$app"
done
if [[ ":$PATH:" != *":$prefix/bin:"* ]]; then
    echo "Note: $prefix/bin is not in your PATH (MonkeyByte's desktop entry uses an absolute path, so the launcher still works)."
fi
