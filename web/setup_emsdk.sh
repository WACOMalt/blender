#!/usr/bin/env bash
# Install and activate a pinned Emscripten SDK (idempotent).
# Usage: source web/setup_emsdk.sh   (or run it, then source $EMSDK/emsdk_env.sh)
set -euo pipefail

EMSDK_VERSION="${EMSDK_VERSION:-3.1.74}"
EMSDK_DIR="${EMSDK:-$HOME/.emsdk}"

if [ ! -d "$EMSDK_DIR" ]; then
  git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
fi
"$EMSDK_DIR/emsdk" install "$EMSDK_VERSION"
"$EMSDK_DIR/emsdk" activate "$EMSDK_VERSION"
# shellcheck disable=SC1091
source "$EMSDK_DIR/emsdk_env.sh"
emcc --version | head -1
