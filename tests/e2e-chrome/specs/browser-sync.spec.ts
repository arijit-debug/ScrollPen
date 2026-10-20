import { test, expect } from '../support/fixtures';
import { EXTENSION_ID } from '../support/mock-host.mjs';

// Gate for Phase 0: the extension's reported offset must match the page exactly,
// because the desktop app draws ink at (anchor - scrollY).

test('says hello with the extension version', async ({ host }) => {
  const hello = await host.waitFor((m) => m.type === 'hello');
  expect(hello.v).toBe(1);
  expect(hello.extVersion).toMatch(/^\d+\.\d+\.\d+$/);
});

test('has the pinned extension ID and confirms the app answered', async ({ serviceWorker, host }) => {
  // The app and the native helper accept only this ID (fixed by the manifest "key").
  expect(new URL(serviceWorker.url()).host).toBe(EXTENSION_ID);
  await host.waitFor((m) => m.type === 'hello');
  await expect
    .poll(() => serviceWorker.evaluate(() => (globalThis as { scrollpenLink?: () => object }).scrollpenLink?.()))
    .toEqual({ transport: 'websocket', ready: true, welcomed: true });
});

test('reports the document on load', async ({ context, host, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  const nav = await host.waitFor((m) => m.type === 'navigate' && m.docKey?.endsWith('/long-article.html'));
  expect(nav.tabId).toBeGreaterThan(0);
  const first = await host.waitFor((m) => m.type === 'scroll');
  expect(first.scrollY).toBe(0);
  expect(first.scrollHeight).toBeGreaterThanOrEqual(20000);
});

test('programmatic scroll is reported exactly', async ({ context, host, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await host.waitFor((m) => m.type === 'scroll');

  for (const y of [500, 2000, 7350, 0]) {
    await page.evaluate((top) => window.scrollTo(0, top), y);
    const msg = await host.waitFor((m) => m.type === 'scroll' && m.scrollY === y);
    expect(msg.scroller).toBe('document');
  }
});

test('no drift after 10 screens of wheel scrolling', async ({ context, host, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await host.waitFor((m) => m.type === 'scroll');
  await page.mouse.move(640, 400);

  // ~10 viewports down in wheel notches, then half of it back up.
  for (let i = 0; i < 70; i++) await page.mouse.wheel(0, 120);
  for (let i = 0; i < 35; i++) await page.mouse.wheel(0, -120);

  await expect
    .poll(async () => {
      const actual = await page.evaluate(() => window.scrollY);
      return Math.abs((host.lastScroll()?.scrollY ?? -1) - actual);
    })
    .toBe(0);
  expect(await page.evaluate(() => window.scrollY)).toBeGreaterThan(3000);
});

test('inner scrolling elements are reported with their own key and rect', async ({ context, host, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/inner-scroller.html`);
  await host.waitFor((m) => m.type === 'scroll');

  await page.evaluate(() => document.getElementById('pane')!.scrollTo(0, 1200));
  const msg = await host.waitFor((m) => m.type === 'scroll' && m.scroller !== 'document');
  expect(msg.scroller).toBe('#pane');
  expect(msg.scrollY).toBe(1200);
  expect(msg.scrollerRect.x).toBe(240);
});

test('browser zoom changes are reported', async ({ context, host, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  const nav = await host.waitFor((m) => m.type === 'navigate' && !!m.docKey);

  await serviceWorker.evaluate((tabId) => chrome.tabs.setZoom(tabId, 1.5), nav.tabId);
  const zoom = await host.waitFor((m) => m.type === 'zoom' && m.zoom === 1.5);
  expect(zoom.tabId).toBe(nav.tabId);
});

test('switching tabs is reported', async ({ context, host, site }) => {
  const a = await context.newPage();
  await a.goto(`${site.origin}/long-article.html`);
  const b = await context.newPage();
  await b.goto(`${site.origin}/inner-scroller.html`);
  host.clear();

  await a.bringToFront();
  const focus = await host.waitFor((m) => m.type === 'focus');
  expect(focus.windowId).toBeGreaterThan(0);
});

test('a normal web page cannot connect to the app port', async ({ context, host, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  const result = await page.evaluate(
    (port) =>
      new Promise<string>((resolve) => {
        const ws = new WebSocket(`ws://127.0.0.1:${port}`);
        ws.onopen = () => resolve('open');
        ws.onerror = () => resolve('rejected');
      }),
    host.port,
  );
  expect(result).toBe('rejected');
});
