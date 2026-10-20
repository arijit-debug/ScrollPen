// Interactive sandbox: opens a real Chromium window with the extension loaded,
// the fixture pages served locally, and the mock host printing every message.
// Usage: npm run chrome:sandbox   (close the browser window to stop)
import { chromium } from '@playwright/test';
import { mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startMockHost } from './support/mock-host.mjs';
import { startStaticServer } from './support/static-server.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(here, '../../extensions/browser-sync/dist');
const profileDir = join(tmpdir(), 'scrollpen-chrome-sandbox');
mkdirSync(profileDir, { recursive: true });

const site = await startStaticServer(resolve(here, 'fixtures'));
const host = await startMockHost(0, { log: true });

const context = await chromium.launchPersistentContext(profileDir, {
  channel: 'chromium',
  headless: false,
  viewport: null,
  args: [`--disable-extensions-except=${extensionDir}`, `--load-extension=${extensionDir}`],
});
const sw = context.serviceWorkers()[0] ?? (await context.waitForEvent('serviceworker'));
await sw.evaluate((port) => chrome.storage.local.set({ port }), host.port);

const page = context.pages()[0] ?? (await context.newPage());
await page.goto(`${site.origin}/long-article.html`);
console.log(`Fixtures: ${site.origin}/long-article.html, ${site.origin}/inner-scroller.html`);
console.log(`Mock host: ws://127.0.0.1:${host.port}. Scroll, zoom and switch tabs; messages print below.`);

context.on('close', async () => {
  await host.close();
  await site.close();
  process.exit(0);
});
