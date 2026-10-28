#!/usr/bin/env bash
# Configure and build Blender for WebAssembly.
#
# Usage:
#   source web/setup_emsdk.sh
#   web/build_deps.sh
#   web/build_blender.sh [extra cmake -D options ...]
#
# Environment:
#   BLENDER_WEB_DEPS   same as for build_deps.sh (default: $HOME/blender-web-deps)
#   BLENDER_WEB_BUILD  build directory (default: ../build_web next to the source tree)
#   JOBS               parallel jobs (default: nproc)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="${BLENDER_WEB_DEPS:-$HOME/blender-web-deps}"
BUILD="${BLENDER_WEB_BUILD:-$ROOT/../build_web}"
JOBS="${JOBS:-$(nproc)}"

command -v emcmake >/dev/null || { echo "emcmake not found; run: source web/setup_emsdk.sh" >&2; exit 1; }
[ -d "$DEPS/install" ] || { echo "No dependencies in $DEPS/install; run web/build_deps.sh" >&2; exit 1; }

CCACHE_ARGS=()
if command -v ccache >/dev/null; then
  CCACHE_ARGS=(-DWITH_COMPILER_CCACHE=ON)
fi

emcmake cmake -S "$ROOT" -B "$BUILD" -G Ninja \
  -C "$ROOT/build_files/cmake/config/blender_web.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DEMSCRIPTEN_SYSTEM_PROCESSOR=wasm32 \
  -DCMAKE_PREFIX_PATH="$DEPS/install" \
  -DCMAKE_FIND_ROOT_PATH="$DEPS/install" \
  "${CCACHE_ARGS[@]}" \
  "$@"
cmake --build "$BUILD" -j "$JOBS"
