#!/usr/bin/env bash
# Run the A2A Python SDK smoke test against a freshly started daedalus-cli
# A2A server. Intended to be driven from CMake target `a2a_smoke_test`.
#
# Usage:
#   scripts/run_a2a_smoke_test.sh <path-to-daedalus-cli>

set -euo pipefail

DAEDALUS_CLI="${1:-}"
if [[ -z "$DAEDALUS_CLI" ]]; then
    echo "Usage: $0 <path-to-daedalus-cli>" >&2
    exit 1
fi

if [[ ! -x "$DAEDALUS_CLI" ]]; then
    echo "Not executable: $DAEDALUS_CLI" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_PYTHON="$SCRIPT_DIR/.venv/bin/python"
if [[ ! -x "$VENV_PYTHON" ]]; then
    echo "Python venv not found at $SCRIPT_DIR/.venv" >&2
    echo "Create it with: uv venv scripts/.venv && uv pip install --python scripts/.venv/bin/python a2a-sdk" >&2
    exit 1
fi

PROJECT_DIR="$(mktemp -d)"
SERVER_LOG="$(mktemp)"
trap 'rm -rf "$PROJECT_DIR" "$SERVER_LOG"; kill "$SERVER_PID" 2>/dev/null || true' EXIT

"$DAEDALUS_CLI" --a2a-server --a2a-port 0 "$PROJECT_DIR" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

PORT=""
for _ in $(seq 1 50); do
    PORT=$(grep -oE 'http://127\.0\.0\.1:[0-9]+' "$SERVER_LOG" | head -n1 | sed 's/.*://' || true)
    if [[ -n "$PORT" ]]; then break; fi
    sleep 0.1
done

if [[ -z "$PORT" ]]; then
    echo "A2A server failed to start or report its port" >&2
    cat "$SERVER_LOG" >&2 || true
    exit 1
fi

echo "A2A smoke test against http://127.0.0.1:$PORT"
"$VENV_PYTHON" "$SCRIPT_DIR/test_a2a_sdk.py" "http://127.0.0.1:$PORT"
