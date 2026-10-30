#!/usr/bin/env node
// Run the WebAssembly build of Blender with Node.js, for testing without a browser.
//
// Usage: node web/run_node.js <build>/bin/blender.js [blender arguments...]
//
// The host file-system is mounted at the same paths (only directories under the
// current working directory and the Blender build directory), so Blender finds its
// data files next to `blender.js` and relative paths work as usual.
'use strict';
const path = require('node:path');

const blenderJs = path.resolve(process.argv[2] || '');
if (!process.argv[2] || !blenderJs.endsWith('.js')) {
  console.error('Usage: node web/run_node.js <build>/bin/blender.js [blender arguments...]');
  process.exit(1);
}

/** Create `dir` (and parents) in the Emscripten file-system and mount the host directory. */
function mountHostDir(FS, NODEFS, dir) {
  let current = '';
  for (const part of dir.split('/').filter(Boolean)) {
    current += '/' + part;
    if (!FS.analyzePath(current).exists) {
      FS.mkdir(current);
    }
  }
  FS.mount(NODEFS, {root: dir}, dir);
}

const mounts = [...new Set([path.dirname(blenderJs), process.cwd()])]
  // Mount parents before children, skip directories already covered by a parent mount.
  .sort((a, b) => a.length - b.length)
  .filter((dir, i, all) => !all.slice(0, i).some(parent => dir.startsWith(parent + '/')));

const config = {
  thisProgram: blenderJs,
  arguments: process.argv.slice(3),
  preRun: [
    module => {
      const FS = module.FS;
      for (const dir of mounts) {
        mountHostDir(FS, module.NODEFS, dir);
      }
      FS.chdir(process.cwd());
      /* Pass the host environment, e.g. for Blender's debug variables. */
      Object.assign(module.ENV, process.env);
      /* Blender prefers `$PWD` over `getcwd()` for the working directory. */
      module.ENV.PWD = process.cwd();
    },
  ],
};
require(blenderJs)(config);
