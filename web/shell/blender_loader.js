// SPDX-FileCopyrightText: 2026 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Start the WebAssembly build of Blender in the page (see `index.html`).
//
// Blender's run-time files are packed by `web/package_data.py` into `blender-data.bin`
// (with the `blender-data.json` index) and written to the in-memory file-system before `main()`.
//
// The user's home directory (preferences, saved files) is kept in the browser's IndexedDB.
// Files can be dropped on the page to open them, the "Files" panel downloads and uploads files.
//
// Command line arguments can be passed for testing, e.g. `?args=-b,--python-expr,print(1)`,
// `?fetch=` adds files from the server to Blender's file-system, `?env=` sets environment variables.
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

  /* Blender's home directory, persistent in IndexedDB. */
  const HOME = '/home/web_user';
  let module = null;
  let syncing = false;

  /** Save the home directory to IndexedDB. */
  function syncHome() {
    if (!module || syncing) {
      return;
    }
    syncing = true;
    module.FS.syncfs(false, error => {
      syncing = false;
      if (error) {
        console.error('Saving files to IndexedDB failed:', error);
      }
    });
  }

  const config = {
    canvas: document.getElementById('canvas'),
    arguments: args,
    print: text => console.log(text),
    printErr: text => console.error(text),
    preRun: [
      instance => {
        module = instance;
        const FS = module.FS;
        for (const [path, offset, size] of index.files) {
          FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
          FS.writeFile(path, data.subarray(offset, offset + size));
        }
        data = null;
        /* Environment variables for testing, e.g. `?env=BLENDER_WEBGPU_EEVEE=1,BLENDER_DEBUG=1`. */
        for (const entry of params.has('env') ? params.get('env').split(',') : []) {
          const [name, ...value] = entry.split('=');
          module.ENV[name] = value.join('=');
        }
        /* Load the home directory from IndexedDB before Blender starts. */
        FS.mkdirTree(HOME);
        FS.mount(module.IDBFS, {}, HOME);
        module.addRunDependency('idbfs');
        FS.syncfs(true, error => {
          if (error) {
            console.error('Loading files from IndexedDB failed:', error);
          }
          module.removeRunDependency('idbfs');
        });
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
      setInterval(syncHome, 10000);
      document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') {
          syncHome();
        }
      });
      setupFiles();
    },
    onAbort: reason => {
      overlay.classList.remove('hidden');
      fail(`Blender stopped: ${reason}`);
    },
  };
  /** Write browser `File`s to the home directory, returns their paths. */
  async function writeFiles(files) {
    const paths = [];
    for (const file of files) {
      const path = `${HOME}/${file.name}`;
      module.FS.writeFile(path, new Uint8Array(await file.arrayBuffer()));
      paths.push(path);
    }
    syncHome();
    return paths;
  }

  /** Drag and drop on the canvas, and the "Files" panel. */
  function setupFiles() {
    const canvas = document.getElementById('canvas');
    canvas.addEventListener('dragover', event => event.preventDefault());
    canvas.addEventListener('drop', async event => {
      event.preventDefault();
      const paths = await writeFiles(event.dataTransfer.files);
      if (paths.length) {
        const rect = canvas.getBoundingClientRect();
        const scale = window.devicePixelRatio || 1;
        module.ccall('ghost_web_drop_files', null, ['number', 'number', 'string'],
                     [Math.round((event.clientX - rect.left) * scale),
                      Math.round((event.clientY - rect.top) * scale), paths.join('\n')]);
      }
    });

    const button = document.getElementById('files-button');
    const panel = document.getElementById('files-panel');
    const list = document.getElementById('files-list');
    const upload = document.getElementById('files-upload');
    button.classList.remove('hidden');

    function listFiles(dir, result) {
      for (const name of module.FS.readdir(dir)) {
        if (name === '.' || name === '..' || name === '.config') {
          continue;
        }
        const path = `${dir}/${name}`;
        const stat = module.FS.stat(path);
        if (module.FS.isDir(stat.mode)) {
          listFiles(path, result);
        }
        else {
          result.push([path, stat.size]);
        }
      }
      return result;
    }
    function refresh() {
      list.replaceChildren();
      const files = listFiles(HOME, []);
      if (!files.length) {
        list.textContent = 'No files yet. Files saved by Blender in its home directory appear here.';
      }
      for (const [path, size] of files) {
        const row = document.createElement('div');
        row.className = 'files-row';
        const link = document.createElement('a');
        link.textContent = path.substring(HOME.length + 1);
        link.href = '#';
        link.title = 'Download';
        link.addEventListener('click', event => {
          event.preventDefault();
          const blob = new Blob([module.FS.readFile(path)]);
          const a = document.createElement('a');
          a.href = URL.createObjectURL(blob);
          a.download = path.substring(path.lastIndexOf('/') + 1);
          a.click();
          setTimeout(() => URL.revokeObjectURL(a.href), 10000);
        });
        const sizeText = document.createElement('span');
        sizeText.textContent = `${(size / 1e6).toFixed(2)} MB`;
        row.append(link, sizeText);
        list.append(row);
      }
    }
    button.addEventListener('click', () => {
      panel.classList.toggle('hidden');
      if (!panel.classList.contains('hidden')) {
        refresh();
      }
    });
    document.getElementById('files-close').addEventListener('click', () => {
      panel.classList.add('hidden');
      canvas.focus();
    });
    upload.addEventListener('change', async () => {
      await writeFiles(upload.files);
      upload.value = '';
      refresh();
    });
  }

  try {
    await BlenderModule(config);
  }
  catch (error) {
    overlay.classList.remove('hidden');
    fail(`Blender failed to start: ${error.message || error}`);
  }
})();
