// Usage: node web/tools/run_page.js <url> [screenshot.png] [timeout ms]
// Needs Playwright (pre-installed in cloud sessions). Uses a software WebGPU adapter (SwiftShader).
// Load a page in headless Chromium with WebGPU, print its console, wait for "RESULT:", screenshot.
const { chromium } = require('playwright');
(async () => {
  const [url, shot, timeoutMs] = [process.argv[2], process.argv[3], Number(process.argv[4] || 60000)];
  const browser = await chromium.launch({
    args: ['--enable-unsafe-webgpu', '--enable-features=Vulkan', '--use-vulkan=swiftshader',
           '--use-webgpu-adapter=swiftshader', '--use-angle=swiftshader', '--ignore-gpu-blocklist'],
  });
  const page = await browser.newPage();
  let done;
  const finished = new Promise(r => (done = r));
  page.on('console', m => { console.log(`[${m.type()}] ${m.text()}`); if (m.text().includes('RESULT:')) done(); });
  page.on('pageerror', e => { console.log(`[pageerror] ${e.message}`); });
  await page.goto(url);
  console.log('crossOriginIsolated:', await page.evaluate(() => crossOriginIsolated), 'gpu:', await page.evaluate(() => !!navigator.gpu));
  await Promise.race([finished, new Promise(r => setTimeout(r, timeoutMs))]);
  await page.waitForTimeout(500);
  if (shot) await page.screenshot({ path: shot });
  await browser.close();
})();
