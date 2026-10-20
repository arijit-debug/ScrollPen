// Opens Playwright Chromium (with the browser-sync extension) on a fixture page, talking to
// the REAL ScrollPen app on its default port. Writes the page's geometry and live scrollY to
// a JSON state file every 100 ms until the browser is closed or `seconds` elapse.
// Usage: node chromium-host.mjs <stateFile> [page] [seconds]
import { chromium } from '@playwright/test';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStaticServer } from '../e2e-chrome/support/static-server.mjs';

const [stateFile, pageName = 'long-article.html', seconds = '60'] = process.argv.slice(2);
const here = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(here, '../../extensions/browser-sync/dist');
const site = await startStaticServer(resolve(here, '../e2e-chrome/fixtures'));

const context = await chromium.launchPersistentContext(mkdtempSync(join(tmpdir(), 'sp-desktop-')), {
  channel: 'chromium',
  headless: false,
  viewport: null,
  args: [
    `--disable-extensions-except=${extensionDir}`,
    `--load-extension=${extensionDir}`,
    '--window-position=180,30',
    '--window-size=1000,780',
  ],
});
const page = context.pages()[0] ?? (await context.newPage());
await page.goto(`${site.origin}/${pageName}`);
await page.bringToFront();

const deadline = Date.now() + Number(seconds) * 1000;
let closed = false;
context.on('close', () => (closed = true));
while (!closed && Date.now() < deadline) {
  try {
    const s = await page.evaluate(() => ({
      scrollY: window.scrollY,
      dpr: window.devicePixelRatio,
      innerW: window.innerWidth,
      innerH: window.innerHeight,
    }));
    writeFileSync(stateFile, JSON.stringify({ ready: true, ...s }));
  } catch {
    break;
  }
  await new Promise((r) => setTimeout(r, 100));
}
await context.close().catch(() => {});
await site.close();
process.exit(0);
