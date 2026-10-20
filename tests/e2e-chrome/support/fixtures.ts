import { test as base, chromium, type BrowserContext, type Worker } from '@playwright/test';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { startMockHost } from './mock-host.mjs';
import { startStaticServer } from './static-server.mjs';

const here = dirname(fileURLToPath(import.meta.url));
export const EXTENSION_DIR = resolve(here, '../../../extensions/browser-sync/dist');
export const FIXTURES_DIR = resolve(here, '../fixtures');

type MockHost = Awaited<ReturnType<typeof startMockHost>>;

export const test = base.extend<
  { context: BrowserContext; serviceWorker: Worker; host: MockHost },
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
    await use(sw);
  },

  // A fresh mock app per test on a random port; the extension is pointed at it.
  host: async ({ serviceWorker }, use) => {
    const host = await startMockHost(0);
    await serviceWorker.evaluate((port) => chrome.storage.local.set({ port }), host.port);
    await host.waitFor((m) => m.type === 'hello');
    await use(host);
    await host.close();
  },
});

export const expect = test.expect;
