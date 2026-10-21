// Interactive sandbox: opens a real Chromium window with ScrollPen loaded and the fixture
// pages served locally, to try it by hand. Click ScrollPen's puzzle-piece/toolbar button or
// press Alt+Shift+D on a page. Usage: npm run sandbox   (close the browser window to stop)
import { chromium } from '@playwright/test';
import { mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { startStaticServer } from './support/static-server.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const extensionDir = resolve(here, '../../extensions/scrollpen/dist');
const profileDir = join(tmpdir(), 'scrollpen-chrome-sandbox');
mkdirSync(profileDir, { recursive: true });

const site = await startStaticServer(resolve(here, 'fixtures'));
const context = await chromium.launchPersistentContext(profileDir, {
  channel: 'chromium',
  headless: false,
  viewport: null,
  args: [`--disable-extensions-except=${extensionDir}`, `--load-extension=${extensionDir}`],
});
const page = context.pages()[0] ?? (await context.newPage());
await page.goto(`${site.origin}/demo.html`);
console.log('Pages to try:');
for (const p of ['demo.html', 'long-article.html', 'inner-scroller.html']) console.log(`  ${site.origin}/${p}`);

context.on('close', async () => {
  await site.close();
  process.exit(0);
});
