# Blender for the web (WebAssembly)

Scripts to build Blender with [Emscripten](https://emscripten.org/) so that it runs
in a web browser. See `../Web_Build_Plan.md` for the plan and current status.

## Build

Requirements: Linux, CMake 3.20+, Ninja, git, curl, ~25 GB disk. Network access to
GitHub (git clone and release downloads), SourceForge and GitLab.

```bash
source web/setup_emsdk.sh     # installs and activates the pinned Emscripten SDK
web/build_deps.sh             # builds dependencies into ~/blender-web-deps/install
web/build_blender.sh          # configures and builds into ../build_web
```

| Script | What it does |
|---|---|
| `setup_emsdk.sh` | Installs the pinned Emscripten SDK in `~/.emsdk` (`EMSDK_VERSION` to override). |
| `build_smoke.sh` | Compiles a C program and runs it with Node.js, to check the toolchain. |
| `build_deps.sh` | Builds the libraries Blender needs (zlib, png, jpeg, freetype, OpenEXR, OpenImageIO, OpenColorIO, TBB, ...). Pass package names to rebuild only those. |
| `build_blender.sh` | Runs CMake with `build_files/cmake/config/blender_web.cmake` and builds. |

## Run and test

```bash
web/build_bundle.sh                           # page + blender.js/.wasm + data files in ../build_web/bundle
python3 web/tools/serve_isolated.py 8000 ../build_web/bundle   # serves with the headers below
node web/tools/run_page.js http://localhost:8000/ shot.png 120000   # headless Chromium, WebGPU
```

Blender can also run without a browser, for background tasks (Cycles renders, Python):

```bash
node web/run_node.js ../build_web/bin/blender.js -b scene.blend -E CYCLES -f 1
```

Crashes print `wasm-function[N]` frames, `blender.js.symbols` (next to `blender.js`) maps
`N` to function names.

Test scripts in `web/tests/` are copied into the bundle, the page runs one with
`?fetch=tests/NAME.py&args=--python,/tests/NAME.py`. `run_page.js` takes a list of actions
(`click:X,Y;key:Tab;wait:MS;shot:FILE;drop:URL@X,Y`) to drive the UI before the screenshot.

What works: the full UI with WebGPU (`source/blender/gpu/webgpu/`), Workbench and the
overlays, Edit mode, loading and saving `.blend` files (kept in IndexedDB, or by drag and
drop and the Files panel), Python and `bpy`, Cycles on the CPU (all threads, also in the
viewport), image formats of OpenImageIO. EEVEE is not available yet (its shaders can not be
translated to WGSL), Material Preview falls back to Workbench.

Headless Chromium draws with SwiftShader, which drops lines crossing the camera plane
(`web/tests/lineclip.html`); screenshots miss the near part of the grid because of it.

## How the build works

- `build_files/cmake/platform/platform_emscripten.cmake` is used on top of
  `platform_unix.cmake`. All code (Blender and its dependencies) is compiled with
  `-pthread -fwasm-exceptions -msimd128`; objects compiled with other settings can not be linked.
- Build tools that generate code (`makesdna`, `makesrna`, `datatoc`, ...) are compiled
  to WebAssembly too and run with Node.js (`-sNODERAWFS -sEXECUTABLE`). This way
  `makesdna` gets the struct layout of the real target, without a native build.
- `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`), the browser main thread handles DOM events
  and WebGPU. GPU commands are recorded on Blender's threads and executed on the main thread.
- Shaders are compiled from GLSL to WGSL inside Blender by glslang and Tint
  (`web/shader_compiler/`, built by `build_deps.sh` from Dawn's sources).

## Hosting

Threads use `SharedArrayBuffer`, which browsers only allow on cross-origin isolated pages.
The web server must send these headers for the page and all of its files:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Serve `.wasm` files with `Content-Type: application/wasm` (needed for streaming compilation).
Pre-compressing `.wasm` and `.data` with Brotli or gzip reduces the download a lot.

Example nginx configuration:

```nginx
location / {
    add_header Cross-Origin-Opener-Policy same-origin always;
    add_header Cross-Origin-Embedder-Policy require-corp always;
    types { application/wasm wasm; }
    gzip_static on;
}
```
