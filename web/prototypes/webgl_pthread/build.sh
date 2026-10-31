#!/usr/bin/env bash
# Prototype of the presentation model planned for Blender's WebGL2 backend:
# `main()` runs a blocking loop on a pthread (PROXY_TO_PTHREAD) and draws with WebGL2 on that
# thread, into an OffscreenCanvas it created. Frames reach the page as ImageBitmaps.
#
#   web/prototypes/webgl_pthread/build.sh
#   python3 web/tools/serve_isolated.py 8766 web/prototypes/webgl_pthread &
#   node web/tools/run_page.js http://127.0.0.1:8766/index.html shot.png
#
# Expected output: "RESULT: 30 frames presented, center pixel 255 128 64 255" and an orange canvas.
set -euo pipefail
cd "$(dirname "$0")"
em++ -O2 -pthread -fwasm-exceptions webgl_pthread.cc \
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=0 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 \
  -o webgl_pthread.js
