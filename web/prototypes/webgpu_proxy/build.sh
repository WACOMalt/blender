#!/usr/bin/env bash
# Prototype of the threading model planned for Blender's WebGPU backend:
# `main()` runs a blocking loop on a pthread (PROXY_TO_PTHREAD), WebGPU runs on the browser
# main thread, the pthread waits for asynchronous WebGPU results (buffer mapping) via proxying.
#
#   web/prototypes/webgpu_proxy/build.sh
#   python3 web/tools/serve_isolated.py 8765 &   # from web/prototypes/webgpu_proxy
#   node web/tools/run_page.js http://127.0.0.1:8765/index.html shot.png
#
# Expected output: "RESULT: readback 64 128 191 255" and an orange canvas.
set -euo pipefail
cd "$(dirname "$0")"
em++ -O2 -pthread -fwasm-exceptions webgpu_proxy.cc --use-port=emdawnwebgpu \
  -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=0 -o webgpu_proxy.js
