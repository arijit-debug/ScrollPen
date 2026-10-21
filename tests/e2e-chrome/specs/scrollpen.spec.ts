import {
  command,
  expect,
  inkNear,
  pixel,
  savedInk,
  setSettings,
  stroke,
  test,
  toggle,
} from '../support/fixtures';

// Ink is checked in real screenshots: a green stroke must sit on the same content after
// scrolling, reloading and so on.

test('ink drawn on a page scrolls with the page', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  expect(await inkNear(page, 500, 300)).toBe(true);

  // The mouse wheel keeps scrolling the page while drawing.
  await page.mouse.move(500, 500);
  await page.mouse.wheel(0, 200);
  await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(200);
  expect(await inkNear(page, 500, 100)).toBe(true);
  expect(await inkNear(page, 500, 300)).toBe(false);

  // Stored in document coordinates (CSS px from the top of the page).
  await expect.poll(async () => (await savedInk(serviceWorker, `${site.origin}/long-article.html`))?.items.length).toBe(1);
  const saved = await savedInk(serviceWorker, `${site.origin}/long-article.html`);
  expect(saved!.items[0].layer).toBe('doc');
  expect(saved!.items[0].points[1]).toBeCloseTo(300, 0);
});

test('ink stays attached at a different browser zoom', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 400);
  await serviceWorker.evaluate(async (url) => {
    const tab = (await chrome.tabs.query({})).find((t) => t.url === url);
    await chrome.tabs.setZoom(tab!.id!, 0.5);
  }, page.url());
  // At 50% zoom the content (and the ink with it) is drawn at half the distance.
  await expect.poll(() => page.evaluate(() => window.devicePixelRatio)).toBe(0.5);
  await expect.poll(() => inkNear(page, 250, 200)).toBe(true);
});

test('saved ink comes back when the page is opened again', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 350);
  await expect.poll(async () => (await savedInk(serviceWorker, `${site.origin}/long-article.html`))?.items.length).toBe(1);

  await page.reload();
  await expect.poll(() => inkNear(page, 500, 350)).toBe(true);
  // The toolbar isn't open after a reload, so the page works normally.
  await page.mouse.click(500, 350);
});

test('undo and redo from the keyboard', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  expect(await inkNear(page, 500, 300)).toBe(true);
  await page.keyboard.press('Control+z');
  await expect.poll(() => inkNear(page, 500, 300)).toBe(false);
  await page.keyboard.press('Control+y');
  await expect.poll(() => inkNear(page, 500, 300)).toBe(true);
});

test('the eraser removes a stroke it touches', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  await page.keyboard.press('e');
  await page.mouse.move(500, 250);
  await page.mouse.down();
  await page.mouse.move(500, 350, { steps: 10 });
  await page.mouse.up();
  await expect.poll(() => inkNear(page, 400, 300)).toBe(false);
  // Undo brings it back.
  await page.keyboard.press('Control+z');
  await expect.poll(() => inkNear(page, 400, 300)).toBe(true);
});

test('ink on an inner scrolling panel follows that panel', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/inner-scroller.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 400, 800, 400);
  await page.mouse.move(600, 600);
  await page.mouse.wheel(0, 300);
  await expect.poll(() => page.evaluate(() => document.getElementById('pane')!.scrollTop)).toBe(300);
  await expect.poll(() => inkNear(page, 600, 100)).toBe(true);
  expect(await inkNear(page, 600, 400)).toBe(false);
  await expect
    .poll(async () => (await savedInk(serviceWorker, `${site.origin}/inner-scroller.html`))?.items[0]?.layer)
    .toBe('el:#pane');
});

test('with "follow scrolling" off, ink stays put on the screen', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await setSettings(serviceWorker, { documentMode: false });
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  await page.mouse.move(500, 500);
  await page.mouse.wheel(0, 200);
  await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(200);
  expect(await inkNear(page, 500, 300)).toBe(true);
});

test('whiteboard covers the page and keeps its own ink', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await page.keyboard.press('w');
  await expect.poll(() => pixel(page, 100, 600)).toEqual([251, 251, 248]);
  await stroke(page, 300, 700, 300);
  expect(await inkNear(page, 500, 300)).toBe(true);
  await page.keyboard.press('k'); // blackboard
  await expect.poll(() => pixel(page, 100, 600)).toEqual([29, 42, 36]);
  await page.keyboard.press('k'); // back to the page: the board's ink goes with the board
  await expect.poll(async () => (await pixel(page, 100, 600)).join()).not.toBe('29,42,36');
  expect(await inkNear(page, 500, 300)).toBe(false);
});

test('text can be written on the page', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await page.keyboard.press('t');
  await page.mouse.click(300, 300);
  await page.keyboard.type('Hello');
  await page.keyboard.press('Enter');
  await page.keyboard.type('world');
  await page.keyboard.press('Control+Enter');
  await expect
    .poll(async () => (await savedInk(serviceWorker, `${site.origin}/long-article.html`))?.items[0]?.text)
    .toBe('Hello\nworld');
});

test('shapes, colours by number and sizes by bracket keys', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await page.keyboard.press('r');
  await page.mouse.move(300, 200);
  await page.mouse.down();
  await page.mouse.move(600, 400, { steps: 5 });
  await page.mouse.up();
  await page.keyboard.press('a');
  await page.mouse.move(300, 500);
  await page.mouse.down();
  await page.mouse.move(600, 600, { steps: 5 });
  await page.mouse.up();
  await page.keyboard.press('2'); // second palette colour
  await page.keyboard.press(']');
  await expect
    .poll(async () => {
      const { settings } = await serviceWorker.evaluate(() => chrome.storage.local.get('settings'));
      return [settings.color, settings.sizes.shape];
    })
    .toEqual(['#FB8C00', 5]);
  const saved = await savedInk(serviceWorker, `${site.origin}/long-article.html`);
  expect(saved!.items.map((i) => i.kind)).toEqual(['rect', 'arrow']);
});

test('shortcuts: hide ink and clear', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  await command(serviceWorker, page, 'toggle-ink');
  await expect.poll(() => inkNear(page, 500, 300)).toBe(false);
  await command(serviceWorker, page, 'toggle-ink');
  await expect.poll(() => inkNear(page, 500, 300)).toBe(true);
  await command(serviceWorker, page, 'clear');
  await expect.poll(() => inkNear(page, 500, 300)).toBe(false);
  await expect.poll(() => savedInk(serviceWorker, `${site.origin}/long-article.html`)).toBeUndefined();
});

test('holding the click-through key lets the page get clicks', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await page.evaluate(() => {
    (window as unknown as { clicks: number }).clicks = 0;
    document.addEventListener('click', (e) => {
      if (e.target !== document.querySelector('scrollpen-ink')) (window as unknown as { clicks: number }).clicks++;
    });
  });
  await setSettings(serviceWorker, { clickThroughKey: 'F8' });
  await toggle(serviceWorker, page);
  await page.keyboard.down('F8');
  await page.mouse.click(500, 300);
  await page.keyboard.up('F8');
  expect(await page.evaluate(() => (window as unknown as { clicks: number }).clicks)).toBe(1);
  // Released: clicks draw again instead.
  await page.mouse.click(500, 320);
  expect(await page.evaluate(() => (window as unknown as { clicks: number }).clicks)).toBe(1);
});

test('web pages cannot see inside ScrollPen or talk to it', async ({ context, serviceWorker, site }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  const view = await page.evaluate(() => {
    const host = document.querySelector('scrollpen-ink');
    return {
      present: !!host,
      shadow: host?.shadowRoot ?? null,
      runtime: typeof (globalThis as { chrome?: { runtime?: { sendMessage?: unknown } } }).chrome?.runtime?.sendMessage,
    };
  });
  expect(view).toEqual({ present: true, shadow: null, runtime: 'undefined' });
});

test('PDFs open in ScrollPen and ink sticks to the pages', async ({ context, serviceWorker, site, extensionId }) => {
  const page = await context.newPage();
  // Not ".pdf": a download manager on the dev PC grabs those (see support/make-pdf.mjs).
  const pdf = `${site.origin}/sample.pdfdata`;
  await page.goto(`chrome-extension://${extensionId}/pdf/viewer.html#${encodeURIComponent(pdf)}`);
  await expect.poll(() => page.locator('#pages canvas').count(), { timeout: 15_000 }).toBeGreaterThan(0);
  expect(await page.locator('#pages .page').count()).toBe(3);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  await page.mouse.move(500, 500);
  await page.mouse.wheel(0, 200);
  await expect.poll(() => page.evaluate(() => window.scrollY)).toBe(200);
  await expect.poll(() => inkNear(page, 500, 100)).toBe(true);
  await expect.poll(async () => (await savedInk(serviceWorker, `pdf:${pdf}`))?.items.length).toBe(1);
});

test('the settings page lists saved pages and can delete them', async ({ context, serviceWorker, site, extensionId }) => {
  const page = await context.newPage();
  await page.goto(`${site.origin}/long-article.html`);
  await toggle(serviceWorker, page);
  await stroke(page, 300, 700, 300);
  await expect.poll(async () => (await savedInk(serviceWorker, `${site.origin}/long-article.html`))?.items.length).toBe(1);

  const options = await context.newPage();
  await options.goto(`chrome-extension://${extensionId}/options/options.html`);
  await expect(options.locator('.page .title')).toHaveText('ScrollPen fixture: long article');
  await options.locator('.page button', { hasText: 'Delete' }).click();
  await expect(options.locator('.empty')).toHaveText('No saved ink yet.');
  // The open page drops the deleted ink too.
  await expect.poll(() => inkNear(page, 500, 300)).toBe(false);
});
