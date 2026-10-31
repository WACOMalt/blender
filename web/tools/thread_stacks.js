// SPDX-FileCopyrightText: 2026 Blender Authors
//
// SPDX-License-Identifier: GPL-2.0-or-later

// Find deadlocks: load a page in headless Chromium (from Playwright), wait, then pause every
// worker (Blender's threads) with the DevTools protocol and print their call stacks:
//   node web/tools/thread_stacks.js http://127.0.0.1:8000/ 20000
// `$funcN` is the function with index N in `blender.js.symbols`.
const { spawn } = require('child_process');
const fs = require('fs');

const [url, waitMs] = [process.argv[2], Number(process.argv[3] || 15000)];
const cache = `${process.env.HOME}/.cache/ms-playwright`;
const shellDir = fs.readdirSync(cache).find(d => d.startsWith('chromium_headless_shell'));
const exe = `${cache}/${shellDir}/chrome-headless-shell-linux64/chrome-headless-shell`;
const port = 9333;
const chrome = spawn(exe, [`--remote-debugging-port=${port}`, '--headless', '--enable-unsafe-webgpu',
                           '--enable-features=Vulkan', '--use-vulkan=swiftshader',
                           '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader',
                           '--ignore-gpu-blocklist', '--window-size=1280,720', 'about:blank'],
                     {stdio: 'ignore'});
const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  let version = null;
  for (let i = 0; i < 50 && !version; i++) {
    await sleep(200);
    version = await fetch(`http://127.0.0.1:${port}/json/version`).then(r => r.json()).catch(() => null);
  }
  const ws = new WebSocket(version.webSocketDebuggerUrl);
  await new Promise(r => ws.addEventListener('open', r));
  let id = 0;
  const pending = new Map();
  const listeners = [];
  ws.addEventListener('message', e => {
    const msg = JSON.parse(e.data);
    if (msg.id && pending.has(msg.id)) {
      pending.get(msg.id)(msg);
      pending.delete(msg.id);
    }
    else {
      listeners.forEach(l => l(msg));
    }
  });
  const send = (method, params = {}, sessionId) => new Promise(resolve => {
    const msg = {id: ++id, method, params};
    if (sessionId) {
      msg.sessionId = sessionId;
    }
    pending.set(msg.id, resolve);
    ws.send(JSON.stringify(msg));
  });

  const workers = [];
  listeners.push(msg => {
    if (msg.method === 'Target.attachedToTarget' && msg.params.targetInfo.type === 'worker') {
      workers.push(msg.params.sessionId);
      send('Runtime.runIfWaitingForDebugger', {}, msg.params.sessionId);
    }
  });
  const {result: {targetId}} = await send('Target.createTarget', {url: 'about:blank'});
  const {result: {sessionId: page}} = await send('Target.attachToTarget', {targetId, flatten: true});
  await send('Target.setAutoAttach', {autoAttach: true, waitForDebuggerOnStart: false, flatten: true},
             page);
  await send('Page.navigate', {url}, page);
  await sleep(waitMs);

  for (const [n, session] of workers.entries()) {
    const paused = new Promise(resolve => {
      const l = msg => {
        if (msg.method === 'Debugger.paused' && msg.sessionId === session) {
          resolve(msg.params);
        }
      };
      listeners.push(l);
    });
    await send('Debugger.enable', {}, session);
    send('Debugger.pause', {}, session);
    const event = await Promise.race([paused, sleep(3000).then(() => null)]);
    if (event) {
      console.log(`worker ${n}: ` + event.callFrames.map(f => f.functionName).join(' < '));
      await send('Debugger.resume', {}, session);
    }
    else {
      console.log(`worker ${n}: (no pause)`);
    }
  }
  chrome.kill();
  process.exit(0);
})();
