#!/usr/bin/env bash
# Assemble the static files to serve: page, loader, Blender's WebAssembly and its data files.
#
# Usage: web/build_bundle.sh [output dir]   (after web/build_blender.sh)
#
# Output (default: ../build_web/bundle): index.html, blender_loader.js, blender.js, blender.wasm,
# blender-data.bin, blender-data.json. Serve them with the headers listed in web/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BLENDER_WEB_BUILD:-$ROOT/../build_web}"
OUT="${1:-$BUILD/bundle}"

cmake --build "$BUILD" --target install > /dev/null
VERSION_DIR="$(find "$BUILD/bin" -mindepth 1 -maxdepth 1 -type d -name '[0-9]*.[0-9]*' | sort | tail -1)"

mkdir -p "$OUT"
cp "$ROOT/web/shell/index.html" "$ROOT/web/shell/blender_loader.js" "$OUT/"
cp "$BUILD/bin/blender.js" "$BUILD/bin/blender.wasm" "$OUT/"
python3 "$ROOT/web/package_data.py" "$VERSION_DIR" "$OUT"
echo "Bundle written to $OUT"
