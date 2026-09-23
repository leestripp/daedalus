#!/usr/bin/env bash
# Run monkeybyte from a build tree with the local GSettings schema.
# §Monorepo (2026-09-24): supports both build modes —
#   standalone:  monkeybyte/build/monkeybyte        (cmake -S monkeybyte -B monkeybyte/build)
#   superbuild:  <root>/build/monkeybyte/monkeybyte (cmake -S . -B build at the repo root)
set -euo pipefail
cd "$(dirname "$0")"
if [[ -x ./build/monkeybyte || -f build/CMakeCache.txt ]]; then
    # Standalone subdir build.
    cmake --build build "$@"
    export GSETTINGS_SCHEMA_DIR="$PWD/build/gschemas"
    exec ./build/monkeybyte
elif [[ -f ../build/monkeybyte/CMakeCache.txt ]]; then
    # Monorepo superbuild at the repo root.
    cmake --build ../build --target monkeybyte "$@"
    export GSETTINGS_SCHEMA_DIR="$PWD/../build/gschemas"
    exec ../build/monkeybyte/monkeybyte
else
    echo "run.sh: no build tree found." >&2
    echo "  superbuild:  (cd .. && cmake -S . -B build && cmake --build build -j\$(nproc))" >&2
    echo "  standalone:  cmake -S . -B build && cmake --build build -j\$(nproc)" >&2
    exit 1
fi
