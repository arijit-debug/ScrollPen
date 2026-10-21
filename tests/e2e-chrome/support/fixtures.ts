import { test as base, chromium, type BrowserContext, type Page, type Worker } from '@playwright/test';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { PNG } from 'pngjs';
import { startStaticServer } from './static-server.mjs';

const here = dirname(fileURLToPath(import.meta.url));
export const EXTENSION_DIR = resolve(here, '../../../extensions/scrollpen/dist');
export const FIXTURES_DIR = resolve(here, '../fixtures');

/** Tests draw in this colour: nothing else on the fixture pages is green. */
export const INK = '#00C853';

export const test = base.extend<
  { context: BrowserContext; serviceWorker: Worker; extensionId: string },
  { site: { origin: string } }
>({
  site: [
    async ({}, use) => {
      const s = await startStaticServer(FIXTURES_DIR);
      await use(s);
      await s.close();
    },
    { scope: 'worker' },
  ],

  // Playwright's bundled Chromium (channel "chromium") still honours --load-extension;
  // branded Google Chrome 137+ does not, so automated runs can't use it.
  context: async ({}, use) => {
    const context = await chromium.launchPersistentContext('', {
      channel: 'chromium',
      viewport: { width: 1280, height: 800 },
      args: [`--disable-extensions-except=${EXTENSION_DIR}`, `--load-extension=${EXTENSION_DIR}`],
    });
    await use(context);
    await context.close();
  },

  serviceWorker: async ({ context }, use) => {
    const sw = context.serviceWorkers()[0] ?? (await context.waitForEvent('serviceworker'));
    // Every test draws in green with a pen thick enough to find in a screenshot.
    await sw.evaluate((color) => chrome.storage.local.set({ settings: { color, sizes: { pen: 6 } } }), INK);
    await use(sw);
  },

  extensionId: async ({ serviceWorker }, use) => {
    await use(new URL(serviceWorker.url()).host);
  },
});

export const expect = test.expect;

/** What clicking ScrollPen's toolbar button does, for this page's tab. */
export async function toggle(sw: Worker, page: Page): Promise<void> {
  await sw.evaluate(async (url) => {
    const tab = (await chrome.tabs.query({})).find((t) => t.url === url);
    if (!tab?.id) throw new Error(`no tab for ${url}`);
    await (globalThis as unknown as { scrollpenToggle: (id: number) => Promise<void> }).scrollpenToggle(tab.id);
  }, page.url());
}

/** Runs a keyboard-shortcut command (Chrome shortcuts can't be pressed from tests). */
export async function command(sw: Worker, page: Page, name: string): Promise<void> {
  await sw.evaluate(
    async ({ url, name }) => {
      const tab = (await chrome.tabs.query({})).find((t) => t.url === url);
      await chrome.tabs.sendMessage(tab!.id!, { type: 'command', name });
    },
    { url: page.url(), name },
  );
}

export async function setSettings(sw: Worker, patch: Record<string, unknown>): Promise<void> {
  await sw.evaluate(async (p) => {
    const { settings } = await chrome.storage.local.get('settings');
    await chrome.storage.local.set({ settings: { ...settings, ...p } });
  }, patch);
}

/** The saved ink for a page's address (the storage key ScrollPen uses), or undefined. */
export async function savedInk(sw: Worker, docKey: string): Promise<{ items: { kind: string; layer: string; points: number[]; text?: string }[] } | undefined> {
  return sw.evaluate(async (key) => (await chrome.storage.local.get(key))[key], `ink:${docKey}`);
}

/** Colour of one screen pixel. */
export async function pixel(page: Page, x: number, y: number): Promise<[number, number, number]> {
  const png = PNG.sync.read(await page.screenshot({ clip: { x, y, width: 1, height: 1 } }));
  return [png.data[0], png.data[1], png.data[2]];
}

export const isInk = ([r, g, b]: number[]) => g > 140 && r < 110 && b < 150;

/** Is there test ink within a few pixels of this row (strokes are smoothed, so allow slack)? */
export async function inkNear(page: Page, x: number, y: number, slack = 3): Promise<boolean> {
  const png = PNG.sync.read(
    await page.screenshot({ clip: { x, y: Math.max(0, y - slack), width: 1, height: slack * 2 + 1 } }),
  );
  for (let i = 0; i < png.height; i++) if (isInk([png.data[i * 4], png.data[i * 4 + 1], png.data[i * 4 + 2]])) return true;
  return false;
}

/** A horizontal stroke with the mouse. */
export async function stroke(page: Page, x1: number, x2: number, y: number): Promise<void> {
  await page.mouse.move(x1, y);
  await page.mouse.down();
  await page.mouse.move(x2, y, { steps: 20 });
  await page.mouse.up();
}
