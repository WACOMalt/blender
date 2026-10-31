# Hosting the web build of Blender

Handover notes for whoever deploys the web build (for example to `blender.bsums.xyz`).
Everything below is about serving static files; nothing runs on the server.

## What this is

Blender (5.x alpha, fork `WACOMalt/blender`, branch `claude/sweet-carson-ikkc8u`) compiled
to WebAssembly with Emscripten. It runs entirely in the browser: the full Blender UI drawn with
WebGPU, Workbench shading, Edit mode, Python/`bpy`, Cycles on the CPU (all cores, viewport and
F12 renders), loading and saving `.blend` files. EEVEE is not available yet (Material Preview
falls back to Workbench, F12 with EEVEE reports an error).

Requirements on the visitor's side:

- A browser with **WebGPU**: Chrome/Edge 113+, Firefox 141+ (Windows, macOS), Safari 26+.
  Without WebGPU the page shows an error.
- A 64-bit machine with a few GB of RAM. The WebAssembly memory grows up to 4 GB.
- The first visit downloads ~75 MB (compressed), later visits use the browser cache.
  Start-up after the download takes about 8 s on the first visit and 4 s on later ones: the
  translated shaders, Python's byte-code and the asset library indexes are kept in IndexedDB
  (times measured with a software GPU, real hardware is faster).

## The bundle

`web/build_bundle.sh` writes a directory of static files (default `../build_web/bundle`):

| File | Size | What it is |
|---|---|---|
| `index.html` | 3 KB | The page: canvas, progress bar, status bar, Files panel. |
| `blender_loader.js` | 9 KB | Downloads the data, starts the module, IndexedDB persistence, drag and drop. |
| `blender.js` | 3 MB | Emscripten runtime (`BlenderModule` factory), also loaded by the worker threads. |
| `blender.wasm` | 92 MB (30 MB gzip) | Blender itself. |
| `blender-data.json` | 135 KB | Index of the files in `blender-data.bin`. |
| `blender-data.bin` | 72 MB (41 MB gzip) | Blender's data files: Python scripts and standard library, fonts, icons, `startup.blend`, assets. |
| `blender.js.symbols` | 23 MB | Optional. Maps `wasm-function[N]` in crash traces to function names; only needed to debug. |
| `tests/` | small | Optional test scripts and pages, see `web/README.md`. |

All URLs in the page are relative, so the bundle can be served from the site root or from any
sub-directory. Brotli compresses noticeably better than gzip (about 22 MB for the `.wasm`).

### Building it

Only needed to update the bundle; the build takes a few hours the first time (Linux, ~25 GB
disk, Node.js, CMake, Ninja; see `web/README.md`):

```bash
source web/setup_emsdk.sh   # Emscripten 6.0.13 into ~/.emsdk
web/fetch_lfs.sh            # Blender's LFS files (fonts, startup.blend) from projects.blender.org
web/build_deps.sh           # dependencies into ~/blender-web-deps/install
web/build_blender.sh        # Blender into ../build_web
web/build_bundle.sh         # the static files into ../build_web/bundle
```

## Serving requirements

1. **HTTPS** (or `localhost`). The features below only work in a secure context.
2. **Cross-origin isolation headers on every response** (the page, the scripts, the `.wasm`,
   the data files). Blender's threads use `SharedArrayBuffer`, which browsers only enable
   with:
   ```
   Cross-Origin-Opener-Policy: same-origin
   Cross-Origin-Embedder-Policy: require-corp
   ```
   With these headers the page can only load resources from its own origin (or ones sent
   with `Cross-Origin-Resource-Policy: cross-origin`). Keep everything on one origin.
3. `blender.wasm` served as `Content-Type: application/wasm`, so the browser compiles it
   while it downloads.
4. Pre-compressed files (`.br` and/or `.gz` next to the originals, served with
   `Content-Encoding`). Compressing 92 MB on the fly for each visitor is not an option.
5. Caching: `blender.js` and `blender.wasm` must come from the same deployment (as must
   `blender-data.json` and `blender-data.bin`), and the file names don't change between
   deployments. Serve everything with `Cache-Control: no-cache`: browsers keep the files but
   revalidate them on each visit (a cheap 304 when unchanged). A long `max-age` makes browsers
   mix the cached `blender.wasm` of a previous deployment with a new `blender.js`, and the page
   fails with errors like "wasmExports.xyz is undefined".

No `Content-Security-Policy` is needed; if one is set it must allow `wasm-unsafe-eval`,
`worker-src 'self'` and `blob:` workers.

### nginx

```nginx
server {
    listen 443 ssl http2;
    server_name blender.bsums.xyz;
    root /var/www/blender;   # the bundle directory
    index index.html;

    # Required for SharedArrayBuffer (threads). `always` also adds them to error responses.
    # Keep all `add_header` here: one in a location would replace these.
    add_header Cross-Origin-Opener-Policy same-origin always;
    add_header Cross-Origin-Embedder-Policy require-corp always;
    # Revalidate every file, see "Caching" above.
    add_header Cache-Control "no-cache" always;

    types {
        application/wasm wasm;
        application/javascript js;
        application/json json;
        application/octet-stream bin symbols;
    }

    # Serve blender.wasm.br / blender.wasm.gz when the browser accepts them.
    brotli_static on;   # needs the ngx_brotli module, otherwise remove this line
    gzip_static on;

    location / {
        try_files $uri $uri/ =404;
    }
}
```

Create the compressed files once per deployment:

```bash
cd /var/www/blender
for f in blender.wasm blender.js blender-data.bin blender-data.json blender_loader.js index.html; do
  gzip -9 -k -f "$f"
  brotli -q 11 -k -f "$f"   # if brotli is installed
done
```

If a reverse proxy or CDN (Cloudflare and the like) sits in front, make sure it passes the two
`Cross-Origin-*` headers through and does not strip `Content-Encoding`.

### Checking a deployment

```bash
curl -sI https://blender.bsums.xyz/ | grep -i cross-origin          # both headers present
curl -sI https://blender.bsums.xyz/blender.wasm | grep -i content-type   # application/wasm
curl -sI -H 'Accept-Encoding: br, gzip' https://blender.bsums.xyz/blender.wasm | grep -i content-encoding
```

In the browser, the page prints `crossOriginIsolated: true gpu: true` in the developer console
on start; `false` for either one means the headers are missing or WebGPU is unavailable.
A headless check from the repository (needs Node.js and Chromium with WebGPU):

```bash
node web/tools/run_page.js https://blender.bsums.xyz/ shot.png 120000
```

## How the page behaves

- `blender_loader.js` downloads `blender-data.json` and `blender-data.bin` with a progress
  bar, then creates the module. Emscripten loads `blender.wasm` by itself (streaming compile)
  and starts one Web Worker per CPU core (`navigator.hardwareConcurrency`), all from
  `blender.js`.
- Blender's home directory (`/home/web_user`: preferences, recent files, saved `.blend`
  files, and the caches under `.cache/`: translated WGSL shaders, Python byte-code, asset
  library indexes) is kept in **IndexedDB** and restored on the next visit. It is synced every
  10 s and when the tab is hidden. Clearing the site data resets Blender to its defaults.
- Dragging (orbiting the view, dragging values, transforms) locks the pointer: the cursor is
  hidden until the button is released, Escape cancels. Browsers ask for permission the first
  time or show a notice.
- `.blend` files (and images) can be dropped on the canvas to open them; the "Files" button
  in the status bar lists the home directory with download and upload.
- URL parameters for testing: `?args=-b,--python-expr,print(1)` (Blender's command line,
  comma separated), `?fetch=tests/x.py` (fetch files into `/tests/`), `?env=NAME=VALUE`.
- Blender runs on a worker thread; the browser's main thread only handles input and WebGPU,
  so a busy Blender does not freeze the tab.
