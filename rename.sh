#!/usr/bin/env bash
# rename.sh — recursive find-and-replace across a directory tree.
# Usage:  ./rename.sh <from> <to> <directory>
#
#   <from>     string to search for
#   <to>       replacement string
#   <directory> root directory to search recursively

set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <from> <to> <directory>" >&2
  exit 1
fi

FROM="$1"
TO="$2"
ROOT="$3"

if [[ ! -d "$ROOT" ]]; then
  echo "ERROR: not a directory: $ROOT" >&2
  exit 1
fi

# Escape special chars in FROM for sed
ESCAPED=$(printf '%s' "$FROM" | sed 's/[[\.*^$+?{|\\()]/\\&/g')

count=0
updated=0

while IFS= read -r -d '' file; do
  # Skip binary files
  encoding=$(file --brief --mime-encoding -- "$file" 2>/dev/null || echo "unknown")
  [[ "$encoding" == "binary" ]] && continue

  if grep -qF -- "$FROM" "$file" 2>/dev/null; then
    sed -i "s/${ESCAPED}/${TO}/g" -- "$file"
    echo "  $file"
    updated=$((updated+1))
  fi
  count=$((count+1))
done < <(find "$ROOT" -type f \
  ! -path "*/_deps/*" \
  ! -path "*/.git/*"   \
  ! -path "*/build/*"  \
  -print0)

echo ""
echo "Scanned $count files — $updated updated in $ROOT"
echo "Replaced: \"$FROM\" → \"$TO\""
