# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ScrollPen is a Chrome extension (Manifest V3) for drawing on web pages and PDFs, with ink attached to the page so it stays on the content while scrolling/zooming and is saved per page. It is published on the Chrome Web Store. It replaced a Windows desktop app (React Native for Windows + C++ engine + native-messaging helper); that app's source is in git history up to commit `224b295`. Leftover untracked build folders of it may exist in `apps/` (gitignored).

## Hard constraints

- **Nothing leaves the user's computer.** No servers, accounts, analytics, crash reporting or remote code. Ink and settings live in `chrome.storage.local` only (not `storage.sync`). The only network fetch is the PDF viewer loading the PDF the user opened. Chrome's own Web Store auto-update is the update channel.
- **Web Store rules:** no remote code, every permission justified in `store/listing.md`; keep that file in sync when permissions change. The manifest must not have a `key` field (the store assigns the ID).
- Feature parity with the retired desktop app was required by the user: every tool, setting and shortcut it had exists here (see "Features" below); don't drop any without asking.

## Commands

The shell is PowerShell on Windows (Bash tool also available). Root is an npm workspace (`extensions/*`, `tests/*`).

```powershell
npm install; npx playwright install chromium
npm run ext:build          # bundle to extensions/scrollpen/dist (esbuild); ext:watch adds source maps
npm test                   # build + Playwright tests (tests/e2e-chrome)
npm run typecheck
npm run ext:pack           # release/scrollpen-<version>.zip (refuses source maps)
npm run store:screenshots  # release/store/*.png, 1280x800, drawn by the real extension
npm run sandbox            # headed Chromium with the extension + fixture pages
npm run ext:icons          # regenerate icons/*.png (scripts/make-icons.mjs, no deps)
```

Single test: `cd tests/e2e-chrome; npx playwright test -g "eraser"`.

Release: bump `version` in both `extensions/scrollpen/manifest.json` and its `package.json`, then `npm run ext:pack`; the user uploads the zip (steps in `store/PUBLISHING.md`).

## Architecture

### Drawing layer (`src/ink/overlay.ts`)
One `Overlay` per page (content script `src/content.ts`, or the PDF viewer). `boot()` runs once per page (global guard: the toolbar button may inject `content.js` into tabs opened before install while Chrome also runs it). The overlay creates its DOM lazily (only when the page has saved ink or the user toggles it): a `<scrollpen-ink>` element appended to `<html>` with inline `!important` styles and a **closed** shadow root (pages can't see in; note `composedPath()` from window listeners can't see in either; use `this.root.activeElement`).

Layers (`Item.layer`, see `types.ts`):
- `doc`: an SVG absolutely positioned at the document origin; items are in document CSS px. The browser scrolls/zooms it with the content natively, so there is no sync code and no lag. The `<g>` is translated by `-(hostRect + scroll)` to cancel any offset of the host's containing block.
- `el:<css path>`: inner scrolling elements (found under the pen by `scrollerAt`, keyed by `cssPath`). The SVG is placed over the element's client box (clipped) and translated by its scroll; re-laid-out on scroll (capture listener), resize, ResizeObserver and DOM mutations (targets that appear late are re-queried).
- `image`: image tabs; scaled by displayed/natural width.
- `screen`: fixed to the viewport ("follow scrolling" off, `settings.documentMode`).
- `board`: whiteboard/blackboard, fixed, shown only while a board is open.

Input: a fixed full-viewport `.capture` div takes pointer events only while drawing (`active`); pointer capture, coalesced events, pen pressure, pen eraser button (buttons & 32). Wheel over the capture layer scrolls the page natively; inner scrollers are scrolled by hand (`onWheel`). The hold-to-click-through key sets `.through` (pointer-events none). Keyboard shortcuts are a window capture-phase listener active only while drawing. Text opens a textarea on pointer **up** (pointer down moves focus and would close it).

Model: `items` plus undo/redo of `Op {add, remove}` (undo swaps them); nudging and erasing produce new ids so `renderAll` (keyed by id) re-renders. Saves are debounced to `chrome.storage.local` key `ink:<docKey>` (`docKey` = origin+path+query; `pdf:<url>` in the viewer); `storage.onChanged` adopts saves from other tabs/the settings page (own saves are recognised by `updated` timestamp set before saving). URL changes in single-page apps are polled (`checkUrl`). Everything read from storage goes through `sanitizeItems`/`sanitizeSettings`.

Rendering (`render.ts`): DOM calls only (never markup strings; text via `textContent`). Pen strokes are perfect-freehand outlines; highlighter is a stroked path at 0.35 opacity; shapes are SVG primitives.

### Service worker (`src/background.ts`)
Toolbar button / `_execute_action` (Alt+Shift+D) → `toggle` message to the tab (injects `content.js` via `scripting` if absent; marks the badge "!" where Chrome forbids extensions). `chrome.commands` (`clear`, `toggle-ink`, `screenshot`, `toggle-mode`) → `command` messages. ScrollPen's own pages (PDF viewer) don't receive `tabs.sendMessage`: they get a `runtime.sendMessage` broadcast with `tabId` and filter on `chrome.tabs.getCurrent()`. Screenshots: `captureVisibleTab` (needs `activeTab`, granted by the button/shortcut) for the sender's active tab only. `globalThis.scrollpenToggle(tabId)` exists for tests.

### PDFs (`src/pdf/`)
`.pdf` URLs (http/https/file) are redirected to `pdf/viewer.html#<url>` when `settings.openPdfs`; Back shows Chrome's viewer once (`chrome.storage.session` key `pdf-native:<tabId>`). The viewer only loads http/https/file, renders with pdf.js 6 (pass bytes as `Uint8Array`; a bare ArrayBuffer reads as empty), no scripting/XFA, under the extension CSP (`script-src 'self'`); pages are laid out once at load so ink never moves. Keep `pdfjs-dist` current (CI fails when it's behind). An empty response means a download manager grabbed the file; the viewer says so.

### Tests (`tests/e2e-chrome`)
Playwright **bundled Chromium** (branded Chrome 137+ ignores `--load-extension`), fixture pages on loopback. Tests draw with the real mouse in green (`#00C853`, set via storage in the fixture) and check screen pixels (`inkNear`, pngjs) plus saved storage. Chrome shortcuts can't be pressed from tests; `command()` sends the same message. On this PC Internet Download Manager intercepts `.pdf` downloads even from 127.0.0.1 (answers 204), so the PDF test uses `fixtures/sample.pdfdata` (same bytes; `support/make-pdf.mjs` writes both).

## Features (parity checklist with the old app)
Pen, highlighter, laser (fade time setting), eraser, line, arrow, rect, ellipse (filled option), text (Enter newline, Ctrl+Enter/Esc finish); 9-colour palette (keys 1-9), sizes per tool (`[` `]`); undo/redo (Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z); clear (Delete); whiteboard W / blackboard K; follow scrolling on/off; hide ink; screenshot; Ctrl+↑/↓ nudge ink; hold-to-click-through key (CapsLock/`/F8/RCtrl/off); minimize toolbar; draggable toolbar; "show toolbar on every page" (old "start active"); configurable shortcuts (chrome://extensions/shortcuts); reset settings. Not possible in an extension: hiding the toolbar from screen capture (use minimize).

## Gotchas
- After changing the manifest or service worker, reload the unpacked extension in `chrome://extensions` and reload open tabs.
- `CSS` is the browser global (`CSS.escape`); don't shadow it (the stylesheet constant is `SHADOW_CSS`).
- In the Bash tool `python3` may resolve to the Windows Store stub; use `python`. Scratch scripts outside the repo must import Playwright by file URL (`file:///E:/ScrollPen/node_modules/@playwright/test/index.mjs`).
- Git: `.gitattributes` forces LF. Commit messages written from PowerShell go through a UTF-8 file without BOM (`git commit -F file`).
- `npm audit` must stay at 0.
