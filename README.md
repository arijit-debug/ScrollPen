# ScrollPen

A Chrome extension for drawing on web pages and PDFs. The ink is attached to the page, so it
stays on the same content while you scroll or zoom, and it is saved for each page so it's
there when you come back. Everything stays on your computer.

Tools: pen (pressure-sensitive), highlighter, laser pointer, eraser, line, arrow, rectangle,
ellipse, text, whiteboard/blackboard, undo/redo, screenshots, nine colours and adjustable sizes.

## Repository layout

```
extensions/scrollpen/     the extension (TypeScript, bundled with esbuild into dist/)
  src/ink/                drawing layer: model, rendering, toolbar, storage
  src/background.ts       toolbar button, shortcuts, screenshots, PDF redirect
  src/pdf/                ScrollPen's PDF viewer (pdf.js)
  src/options/            settings page
  scripts/                icon drawing and Web Store zip packing
tests/e2e-chrome/         Playwright tests that draw with the real extension in Chromium
store/                    Web Store listing text, privacy policy and publishing steps
```

## Development

Node.js 22 or later.

```powershell
npm install
npx playwright install chromium
npm run ext:build      # -> extensions/scrollpen/dist (load it via chrome://extensions > Load unpacked)
npm run ext:watch      # rebuild on change (with source maps)
npm test               # build + all Chrome tests
npm run sandbox        # a Chromium window with ScrollPen and the test pages, to try by hand
npm run typecheck
npm run ext:pack       # -> release/scrollpen-<version>.zip (Chrome Web Store) and
                       #    release/ScrollPen-for-friends-<version>.zip (to share; steps in share/HOW TO INSTALL.txt)
npm run store:screenshots
```

Publishing: see [store/PUBLISHING.md](store/PUBLISHING.md).

## Privacy

No servers, accounts or analytics. Ink and settings live in `chrome.storage.local` in the
user's Chrome profile and can be deleted from the settings page. See
[store/privacy-policy.md](store/privacy-policy.md).
