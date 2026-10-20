// Checks that the extension reaches the REAL running ScrollPen app through Chrome native
// messaging (no test port set). Needs the helper registered for Chromium:
//   pwsh apps/desktop/windows/installer/register-native-host.ps1 -HostExe <ScrollPenHost.exe> -Chromium
// Opens a Playwright Chromium window for a few seconds; moves no mouse or keyboard.
import { chromium } from '@playwright/test';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const extensionDir = resolve(dirname(fileURLToPath(import.meta.url)), '../../extensions/browser-sync/dist');
const context = await chromium.launchPersistentContext(mkdtempSync(join(tmpdir(), 'sp-native-')), {
  channel: 'chromium',
  headless: false,
  args: [`--disable-extensions-except=${extensionDir}`, `--load-extension=${extensionDir}`, '--window-position=200,60'],
});
const sw = context.serviceWorkers()[0] ?? (await context.waitForEvent('serviceworker'));
let status = null;
for (let i = 0; i < 50 && !(status?.ready && status?.welcomed); i++) {
  await new Promise((r) => setTimeout(r, 200));
  status = await sw.evaluate(() => globalThis.scrollpenLink?.());
}
await context.close();
const ok = status?.transport === 'native' && status.ready && status.welcomed;
console.log(`${ok ? 'PASS' : 'FAIL'}  extension -> native helper -> app: ${JSON.stringify(status)}`);
process.exit(ok ? 0 : 1);
