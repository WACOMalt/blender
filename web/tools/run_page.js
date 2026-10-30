// Usage: node web/tools/run_page.js <url> [screenshot.png] [timeout ms] [actions]
// Needs Playwright (pre-installed in cloud sessions). Uses a software WebGPU adapter (SwiftShader).
// Load a page in headless Chromium with WebGPU, print its console, wait for "RESULT:" (or the
// timeout), then run the actions and take a screenshot.
//
// Actions are separated by ';': `click:X,Y`, `move:X,Y`, `key:KEY` (Playwright key names),
// `type:TEXT`, `wait:MS`, `shot:FILE.png`, `reload:` (keeps the browser storage) and
// `drop:URL@X,Y` (drops the file at URL on the canvas).
const { chromium } = require('playwright');
(async () => {
  const [url, shot, timeoutMs, actions] = [process.argv[2], process.argv[3],
                                           Number(process.argv[4] || 60000), process.argv[5] || ''];
  const browser = await chromium.launch({
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
           '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist'],
  });
  const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
  let done;
  const finished = new Promise(r => (done = r));
  page.on('console', m => { console.log(`[${m.type()}] ${m.text()}`); if (m.text().includes('RESULT:')) done(); });
  page.on('pageerror', e => { console.log(`[pageerror] ${e.message}\n${e.stack || ''}`); });
  await page.goto(url);
  console.log('crossOriginIsolated:', await page.evaluate(() => crossOriginIsolated), 'gpu:', await page.evaluate(() => !!navigator.gpu));
  await Promise.race([finished, new Promise(r => setTimeout(r, timeoutMs))]);
  for (const action of actions.split(';').filter(Boolean)) {
    const [name, arg = ''] = [action.slice(0, action.indexOf(':')), action.slice(action.indexOf(':') + 1)];
    console.log(`[action] ${action}`);
    if (name === 'click') {
      const [x, y] = arg.split(',').map(Number);
      await page.mouse.click(x, y);
    }
    else if (name === 'move') {
      const [x, y] = arg.split(',').map(Number);
      await page.mouse.move(x, y);
    }
    else if (name === 'key') {
      await page.keyboard.press(arg);
    }
    else if (name === 'type') {
      await page.keyboard.type(arg);
    }
    else if (name === 'wait') {
      await page.waitForTimeout(Number(arg));
    }
    else if (name === 'shot') {
      await page.screenshot({ path: arg });
    }
    else if (name === 'reload') {
      await page.reload();
      await page.waitForTimeout(timeoutMs);
    }
    else if (name === 'drop') {
      const [fileUrl, position] = arg.split('@');
      const [x, y] = position.split(',').map(Number);
      await page.evaluate(async ([fileUrl, x, y]) => {
        const bytes = await (await fetch(fileUrl)).arrayBuffer();
        const transfer = new DataTransfer();
        transfer.items.add(new File([bytes], fileUrl.substring(fileUrl.lastIndexOf('/') + 1)));
        document.getElementById('canvas').dispatchEvent(
            new DragEvent('drop', { dataTransfer: transfer, clientX: x, clientY: y, bubbles: true }));
      }, [fileUrl, x, y]);
    }
  }
  await page.waitForTimeout(500);
  if (shot) await page.screenshot({ path: shot });
  await browser.close();
})();
