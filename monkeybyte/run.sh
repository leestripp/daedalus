#!/usr/bin/env bash
# Run monkeybyte from the build tree with the local GSettings schema.
set -euo pipefail
cd "$(dirname "$0")"
cmake --build build "$@"
export GSETTINGS_SCHEMA_DIR="$PWD/build/gschemas"
exec ./build/monkeybyte
