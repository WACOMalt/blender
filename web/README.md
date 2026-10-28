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

## How the build works

- `build_files/cmake/platform/platform_emscripten.cmake` is used on top of
  `platform_unix.cmake`. All code (Blender and its dependencies) is compiled with
  `-pthread -fwasm-exceptions -msimd128`; objects compiled with other settings can not be linked.
- Build tools that generate code (`makesdna`, `makesrna`, `datatoc`, ...) are compiled
  to WebAssembly too and run with Node.js (`-sNODERAWFS -sEXECUTABLE`). This way
  `makesdna` gets the struct layout of the real target, without a native build.

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
