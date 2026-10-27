#!/usr/bin/env bash
# Phase 2/3 smoke build: proves the Emscripten toolchain works in this environment.
# Output: build_web_smoke/smoke.{js,wasm}, run with node.
set -euo pipefail
cd "$(dirname "$0")/.."
command -v emcc >/dev/null || { echo "emcc not found; run: source web/setup_emsdk.sh" >&2; exit 1; }
emcmake cmake -S web/smoke -B build_web_smoke -G Ninja
cmake --build build_web_smoke
node build_web_smoke/smoke.js
