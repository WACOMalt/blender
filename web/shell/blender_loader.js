// SPDX-FileCopyrightText: 2026 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Start the WebAssembly build of Blender in the page (see `index.html`).
//
// Blender's run-time files are packed by `web/package_data.py` into `blender-data.bin`
// (with the `blender-data.json` index) and written to the in-memory file-system before `main()`.
//
// Command line arguments can be passed for testing, e.g. `?args=-b,--python-expr,print(1)`,
// `?fetch=` adds files from the server to Blender's file-system.
'use strict';

(async () => {
  const overlay = document.getElementById('overlay');
  const statusText = document.getElementById('status');
  const progressBar = document.getElementById('progress-bar');

  function setStatus(text, fraction) {
    statusText.textContent = text;
    if (fraction !== undefined) {
      progressBar.style.width = `${Math.round(fraction * 100)}%`;
    }
  }
  function fail(text) {
    setStatus(text);
    console.error(text);
  }

  if (!globalThis.crossOriginIsolated) {
    fail('Blender needs a cross-origin isolated page (for threads).\n' +
         'The server must send the "Cross-Origin-Opener-Policy: same-origin" and ' +
         '"Cross-Origin-Embedder-Policy: require-corp" headers.');
    return;
  }

  /** Download `url` into one buffer, reporting progress with `onProgress(loaded)`. */
  async function fetchBytes(url, onProgress) {
    const response = await fetch(url);
    if (!response.ok) {
      throw new Error(`${url}: ${response.status} ${response.statusText}`);
    }
    const reader = response.body.getReader();
    const chunks = [];
    let loaded = 0;
    for (;;) {
      const {done, value} = await reader.read();
      if (done) {
        break;
      }
      chunks.push(value);
      loaded += value.length;
      onProgress(loaded);
    }
    const bytes = new Uint8Array(loaded);
    let offset = 0;
    for (const chunk of chunks) {
      bytes.set(chunk, offset);
      offset += chunk.length;
    }
    return bytes;
  }

  let index, data;
  try {
    index = await (await fetch('blender-data.json')).json();
    data = await fetchBytes('blender-data.bin', loaded => {
      setStatus(`Downloading Blender's data files… ${Math.round(loaded / 1e6)} / ` +
                `${Math.round(index.size / 1e6)} MB`, loaded / index.size);
    });
  }
  catch (error) {
    fail(`Failed to download Blender's data files: ${error.message}`);
    return;
  }
  setStatus('Starting Blender…', 1.0);

  const params = new URLSearchParams(location.search);
  const args = params.has('args') ? params.get('args').split(',') : [];
  /* Extra files for testing, e.g. `?fetch=tests/script.py&args=--python,/tests/script.py`. */
  const extra_files = [];
  for (const path of params.has('fetch') ? params.get('fetch').split(',') : []) {
    extra_files.push([path, new Uint8Array(await (await fetch(path)).arrayBuffer())]);
  }

  const config = {
    canvas: document.getElementById('canvas'),
    arguments: args,
    print: text => console.log(text),
    printErr: text => console.error(text),
    preRun: [
      module => {
        const FS = module.FS;
        for (const [path, offset, size] of index.files) {
          FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
          FS.writeFile(path, data.subarray(offset, offset + size));
        }
        data = null;
        for (const [path, bytes] of extra_files) {
          const fs_path = '/' + path;
          FS.mkdirTree(fs_path.substring(0, fs_path.lastIndexOf('/')));
          FS.writeFile(fs_path, bytes);
        }
      },
    ],
    onRuntimeInitialized: () => {
      overlay.classList.add('hidden');
      document.getElementById('canvas').focus();
    },
    onAbort: reason => {
      overlay.classList.remove('hidden');
      fail(`Blender stopped: ${reason}`);
    },
  };
  try {
    await BlenderModule(config);
  }
  catch (error) {
    overlay.classList.remove('hidden');
    fail(`Blender failed to start: ${error.message || error}`);
  }
})();
