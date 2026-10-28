# Blender WASM Web Build Plan

Goal: keep an up-to-date build of Blender that runs in a desktop web browser
(Chrome and Firefox first), with as many features as are practical.
Hosting/deployment (blender.bsums.xyz) is handled by the repo owner and is out of scope.

Status: **Phase 2 done, Phase 3 in progress.** The Emscripten toolchain, all
dependencies (including CPython 3.13) and the CMake platform support work, see
`web/README.md`. Headless Blender runs under node (loads `.blend` files, Python works).
Cycles (CPU) is being enabled. The windowed build has a GHOST backend for the canvas and a
WebGPU GPU backend in `source/blender/gpu/webgpu/` (written, being brought up in Chromium).

Decisions by the repo owner (2026-10-28):
- GPU: **WebGPU is the primary backend, with a reduced WebGL2 fallback** for browsers without WebGPU.
- No intermediate demo page: go **straight to the full Blender UI** after the headless bring-up.

## 1. Constraints and realities

- Blender is ~C++20, multi-threaded, OpenGL 4.3 / Vulkan oriented, with a large
  dependency tree (`lib/` precompiled libs are native only). None of those
  libraries are available for wasm32 and all must be rebuilt with Emscripten.
- Browser targets: WebGL2 (OpenGL ES 3.0) is universal; WebGPU is Chrome/Edge
  (and recent Firefox) only. Blender's GPU module has OpenGL 4.3+ assumptions
  (compute, SSBOs, image load/store) that WebGL2 does not provide.
- wasm32 limits memory to 4 GB; use `-sMAXIMUM_MEMORY=4GB` and `-sALLOW_MEMORY_GROWTH`.
  wasm64 (`-sMEMORY64`) is an option later but has uneven browser support.
- Threads need SharedArrayBuffer, which requires cross-origin isolation
  (`COOP: same-origin`, `COEP: require-corp`). The host must send these headers.
- No fork/exec, no raw sockets, no native dlopen of arbitrary .so files.

## 2. Target architecture

| Concern | Approach |
|---|---|
| Compiler | Emscripten (pin version in `web/setup_emsdk.sh`), CMake via `emcmake` |
| Config | New `build_files/cmake/config/blender_web.cmake` (headless-style, features OFF by default, enabled incrementally) |
| Threads | pthreads → Web Workers (`-pthread`, `-sPTHREAD_POOL_SIZE`), TBB or Blender's own task scheduler with a fixed pool |
| Graphics (primary) | **WebGPU** backend via Emscripten `emdawnwebgpu`, modeled on the Vulkan backend in `source/blender/gpu/`; compute and storage buffers make EEVEE and the full viewport possible. Browsers only accept WGSL, so shaders need a GLSL → SPIR-V → WGSL path at runtime |
| Graphics (fallback) | Reduced **WebGL2** backend (GLES 3.0, `-sMAX_WEBGL_VERSION=2`) for browsers without WebGPU; no compute or storage buffers, so features are limited |
| Windowing/input | GHOST backend on top of Emscripten HTML5 API (canvas, pointer lock, keyboard, clipboard, IME, DPI) |
| Python | Build CPython for wasm32-emscripten (or use Pyodide's patches); required for the UI, which is Python-defined (`scripts/startup`) |
| Filesystem | Emscripten MEMFS for runtime; **IDBFS** (IndexedDB) for persistent prefs/autosave; **File System Access API** in Chrome for direct open/save of `.blend`; download/upload fallback in Firefox |
| Rendering (Cycles) | CPU Cycles with threads, later; no OSL/Embree/OIDN initially (Embree and OIDN have no wasm port; use Cycles' BVH2 and a fallback denoiser later) |
| Packaging | Output `.wasm` + `.js` + `.data` (preloaded `scripts/` and `datafiles/`), brotli-compressed; lazy-load optional data |
| Browser extras | Chrome: WebGPU, File System Access, OPFS. Firefox: OPFS, WebGPU when stable. Both: feature-detect and degrade |

### GPU findings (survey of `source/blender/gpu/`, 2026-10-28)

- A backend implements `GPUBackend` (`gpu/intern/gpu_backend.hh`) and ~17 abstract classes
  (Context, Batch, Shader, ShaderCompiler, Texture, FrameBuffer, VertBuf, IndexBuf, UniformBuf,
  StorageBuf, QueryPool, StateManager, Fence, Immediate, TexturePool, WorkInFlight, ray tracing AS).
  Sizes for reference: OpenGL ~11k lines, Metal ~24k, Vulkan ~35k.
- All shaders are compiled at **runtime** (built-ins lazily, material node trees via `GPUCodegen`,
  OCIO generates GLSL). WebGPU only accepts WGSL, so a GLSL → SPIR-V → WGSL compiler
  (glslang + Tint, compiled to WebAssembly) must ship in the page, or a WGSL generator be written.
- Compute, storage buffers and image load/store are assumed everywhere (no capability flag).
  Even the **2D UI** needs storage buffers in the vertex stage: text (`gpu_shader_text.bsl.hh`),
  wide/smooth lines (polyline shaders), node links. The **draw manager** (3D viewport) needs
  storage buffers, compute and multi-draw-indirect.
  - WebGPU: read-only storage buffers in vertex shaders are allowed; multi-draw-indirect must be
    emulated with a loop; no geometry shaders (reuse the Metal workaround); no push constants
    (reuse the Vulkan uniform buffer fallback).
  - WebGL2 fallback: needs text and line shaders rewritten without storage buffers, and cannot
    run the draw manager. Expect UI and image editors only, no 3D viewport.
- With no backend, `blender -b` works until something creates a GPU context, then crashes
  (`DummyBackend` returns null objects). CPU-only work (Cycles, scripts) is fine.
- GHOST: base the Emscripten platform on the SDL implementation (smallest interactive one).
  `GHOST_ContextVK` (creates the device in GHOST, hands it to the GPU module) is the model for
  a WebGPU context. `WM_main` loop needs ASYNCIFY or a proxied pthread main thread.

### Threading model (validated by `web/prototypes/webgpu_proxy`, 2026-10-28)

- `main()` runs on a pthread (`-sPROXY_TO_PTHREAD`), so `WM_main` keeps its blocking loop and
  Blender code can wait on locks; the browser main thread stays free.
- WebGPU lives on the **browser main thread**: its asynchronous results (adapter/device,
  buffer mapping for readback, shader compilation info) are promises, which only resolve on a
  thread that returns to its event loop. Blender's thread sends batched work there and blocks
  when it needs a result (`emscripten_proxy_sync_with_ctx`, finished from the WebGPU callback).
  This matches the Vulkan backend's design (render graph recorded on Blender threads, submitted
  separately). The prototype clears the canvas and reads back pixels synchronously in Chromium.
- Input: DOM event handlers (browser main thread) push into a queue in shared memory, GHOST's
  `processEvents` on Blender's thread drains it.
- Testing: `web/tools/serve_isolated.py` (COOP/COEP headers) and `web/tools/run_page.js`
  (headless Chromium with a software WebGPU adapter).

### WebGPU backend design (`source/blender/gpu/webgpu/`)

- All GPU calls record into one command stream (`wg_commands.hh`), executed by `WGDevice` on the
  browser main thread. Objects are ids allocated when recording, so nothing waits for creation.
  Uploads go to the stream's staging data; uniform buffers and push constants are copied into it
  per stream so their updates stay ordered with the draws.
- Shaders: Vulkan style GLSL (same generation as the Vulkan backend) → glslang → SPIR-V → Tint →
  WGSL (`web/shader_compiler/`, compiled to WebAssembly and linked into Blender). Push constants
  become a uniform buffer, samplers are in bind group 1 at the binding of their texture.
- Y is flipped in the vertex shader so textures have the same memory layout as on other
  backends; the window framebuffer is copied to the canvas with a flip when presenting.
- Clears before the first draw of a pass are load operations, other clears are draws.
- Not supported yet: geometry shaders, layered rendering, barycentric coordinates, buffer
  textures, 1D array textures, image atomics, wide points, triangle fans in batches.

## 3. Dependency strategy

Build every library from source with Emscripten into `build_web/deps` through a
script (`web/build_deps.sh`, to be added), cached as a CI/cloud artifact.

Order (smallest blockers first): zlib, zstd, lzma, png, jpeg, freetype,
harfbuzz, fribidi, boost (header-mostly parts), eigen (headers), pugixml,
fmt, OpenEXR/Imath, OpenImageIO (minimal), OpenColorIO, OpenSubdiv (CPU only),
OpenVDB (optional), TBB/pthread shim, Python.

Always OFF initially: CUDA/OptiX/HIP/oneAPI/Metal, Embree, OIDN, OSL, Alembic,
USD, Hydra, FFmpeg, audio (Audaspace/OpenAL → later map to WebAudio), XR, NDOF,
Ceres/libmv (motion tracking), Collada, Draco, MaterialX, Potrace, SDL.

## 4. Phases

### Phase 1: Planning (this document)
- [x] Write this plan.
- [ ] Open the follow-up issues listed in section 6 (as sub-tasks of issue #1).

### Phase 2: Make cloud sessions able to follow the plan
- Pin the Emscripten SDK and install it in cloud sessions (`web/setup_emsdk.sh`);
  consider a SessionStart hook (see the `session-start-hook` skill) that sources it.
- Verify outbound network policy allows emsdk downloads (GitHub, storage.googleapis.com, npm).
- Smoke test: `web/build_smoke.sh` compiles and runs a C program under node.
- Decide where large intermediate artifacts live (CI cache / release assets), since cloud disks are ephemeral.
- Add a CI workflow once the smoke test passes in a cloud session.

### Phase 3: Get something, anything, built
Minimum deployable output, in order of ambition:
1. `makesdna` / `makesrna` host tools built natively (they run at build time; cross-compile requires native host builds).
2. Compile `source/blender/blenlib` + `makesdna` for wasm as static libs (proves the build system change).
3. A tiny `blender_web` target that links the above and runs a function in node, then in a static HTML page.
4. Headless Blender (`blender_headless`-style config) loading a `.blend` and printing its contents in node.
5. Windowed build with Workbench on WebGL2 showing the UI.

**Phase 3 is complete when `build_web/` contains an `index.html`, `.js`, `.wasm`
and `.data` that the owner can serve from a static host.** Stop there.

## 5. Risks

1. GPU module is the largest unknown (compute-dependent features, shader transpilation GLSL→WGSL/ESSL).
2. Python-in-WASM startup time and size; UI is slow to boot without snapshots.
3. Memory ceiling (4 GB) for production scenes.
4. Binary size: expect >100 MB uncompressed; need streaming compile + caching.
5. Upstream merge cost: keep changes behind `WITH_WEB`/`EMSCRIPTEN` guards and isolated in new files where possible.

## 6. Follow-up issues (to be filed)

1. Pin and install Emscripten SDK in cloud sessions; add smoke test CI.
2. Add `blender_web.cmake` config and `EMSCRIPTEN` platform handling in `CMakeLists.txt`/`build_files/cmake/platform/`.
3. Native host-tool pass (`makesdna`, `makesrna`, `datatoc`) for cross-compiling.
4. Dependency build script for wasm (`web/build_deps.sh`), starting with zlib/zstd/png/jpeg/freetype.
5. Build `blenlib` + `dna` for wasm.
6. CPython for wasm32 and `bpy` bring-up.
7. GHOST Emscripten backend (canvas, input, clipboard).
8. GPU: WebGL2 backend, Workbench.
9. Virtual filesystem: IDBFS persistence, File System Access API integration.
10. Threading: pthread pool and cross-origin isolation requirements doc.
11. GPU: WebGPU backend (Chrome/Firefox).
12. Packaging and compression pipeline, `index.html` shell.
