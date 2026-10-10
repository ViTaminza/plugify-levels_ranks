#!/bin/bash
# Assembles build/package/levels_ranks: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/levels_ranks"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/levels_ranks/liblevels_ranks.so" "$OUT/bin/"
cp "$ROOT/build/plugins/levels_ranks/levels_ranks.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
