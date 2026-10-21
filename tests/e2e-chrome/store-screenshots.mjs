// Takes the Chrome Web Store screenshots (1280x800 PNG) with the real extension, by
// drawing on the demo pages with the mouse. Writes release/store/*.png.
// Usage: npm run store:screenshots
import { chromium } from '@playwright/test';
import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStaticServer } from './support/static-server.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(here, '../../extensions/scrollpen/dist');
const out = resolve(here, '../../release/store');
mkdirSync(out, { recursive: true });

const site = await startStaticServer(resolve(here, 'fixtures'));
const context = await chromium.launchPersistentContext('', {
  channel: 'chromium',
  viewport: { width: 1280, height: 800 },
  deviceScaleFactor: 1,
  args: [`--disable-extensions-except=${extensionDir}`, `--load-extension=${extensionDir}`],
});
const sw = context.serviceWorkers()[0] ?? (await context.waitForEvent('serviceworker'));
const id = new URL(sw.url()).host;
const page = context.pages()[0] ?? (await context.newPage());

async function toggle() {
  await sw.evaluate(async (url) => {
    const tab = (await chrome.tabs.query({})).find((t) => t.url === url);
    await globalThis.scrollpenToggle(tab.id);
  }, page.url());
}
const key = (k) => page.keyboard.press(k);
async function path(points) {
  await page.mouse.move(...points[0]);
  await page.mouse.down();
  for (const p of points.slice(1)) await page.mouse.move(p[0], p[1], { steps: 4 });
  await page.mouse.up();
}
const wave = (x1, x2, y, amp = 3) =>
  Array.from({ length: 40 }, (_, i) => [x1 + ((x2 - x1) * i) / 39, y + Math.sin(i / 2.2) * amp]);
const ellipse = (cx, cy, rx, ry) =>
  Array.from({ length: 50 }, (_, i) => [cx + rx * Math.cos((i / 48) * Math.PI * 2 - 0.6), cy + ry * Math.sin((i / 48) * Math.PI * 2 - 0.6)]);
async function text(x, y, s) {
  await key('t');
  await page.mouse.click(x, y);
  await page.keyboard.type(s);
  await key('Control+Enter');
}
async function shot(name) {
  await page.waitForTimeout(300);
  await page.screenshot({ path: join(out, name) });
  console.log(join(out, name));
}

// 1. Annotating an article.
await page.goto(`${site.origin}/demo.html`);
await toggle();
await key('h');
await key('3'); // yellow
await path([[282, 166], [562, 166]]);
await key('p');
await key('1'); // red
await path(ellipse(640, 391, 250, 34));
await path(wave(372, 560, 534, 2.5));
await key('a');
await page.mouse.move(250, 470);
await page.mouse.down();
await page.mouse.move(360, 400, { steps: 6 });
await page.mouse.up();
await key('5'); // blue
await text(70, 480, 'Learn this!');
await key('p');
await shot('1-annotate-a-page.png');

// 2. The same ink after scrolling: it stays on the text.
await page.mouse.move(640, 600);
await page.mouse.wheel(0, 260);
await page.waitForTimeout(400);
await key('4'); // green
await path(wave(282, 600, 528, 2));
await shot('2-ink-stays-when-you-scroll.png');

// 3. A PDF, opened in ScrollPen's viewer.
await page.goto(`chrome-extension://${id}/pdf/viewer.html#${encodeURIComponent(`${site.origin}/sample.pdfdata`)}`);
await page.waitForSelector('#pages canvas', { timeout: 15000 });
await toggle();
await key('h');
await key('3');
await path([[170, 192], [990, 192]]);
await key('r');
await key('1');
await page.mouse.move(160, 290);
await page.mouse.down();
await page.mouse.move(890, 345, { steps: 5 });
await page.mouse.up();
await key('5');
await text(600, 420, 'Check page 2!');
await shot('3-draw-on-pdfs.png');

// 4. The whiteboard.
await page.goto(`${site.origin}/demo.html`);
await toggle();
await key('w');
await key('5');
await key('p');
await path(ellipse(420, 360, 150, 110));
await key('a');
await page.mouse.move(600, 360);
await page.mouse.down();
await page.mouse.move(800, 360, { steps: 6 });
await page.mouse.up();
await key('4');
await key('r');
await page.mouse.move(830, 290);
await page.mouse.down();
await page.mouse.move(1090, 430, { steps: 5 });
await page.mouse.up();
await key('1');
await text(350, 340, 'Idea');
await text(880, 340, 'Result');
await shot('4-whiteboard.png');

await context.close();
await site.close();
