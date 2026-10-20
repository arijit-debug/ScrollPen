# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ScrollPen is a Windows-only screen pen (annotation overlay) whose ink can stay attached to scrolling content. It is a React Native for Windows 0.84.1 app (C++ Fabric host) with a native C++ engine, plus a Chrome/Edge extension that reports page scroll positions to the app.

## Hard constraints

- **Windows only.** No iOS/Android/macOS code, scripts or dependencies (they were deliberately stripped from the RN template).
- **Fully offline after install.** No telemetry, crash-reporting services, auto-update checks, accounts or cloud sync. The extension reaches the app through Chrome native messaging and a per-user named pipe; the only socket is an opt-in loopback WebSocket for tests. Release builds bundle the JS (no Metro). Builds pass `--no-telemetry` to the RNW CLI (`ExperimentalFeatures.props`, npm scripts); keep it that way.
- **All rendering is local** (Direct2D/DirectComposition on the GPU; WARP fallback). Each PC and Windows user is independent: ink lives in memory, settings in `%AppData%\ScrollPen\settings.json`.

## Commands

The shell is PowerShell on Windows. After installing tools, refresh PATH in a new shell: `$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')`.

Root (npm workspaces: `packages/*`, `extensions/*`, `tests/*`; `apps/desktop` is deliberately **not** a workspace and has its own `node_modules`):

```powershell
npm install; npm run app:install; npx playwright install chromium
npm run ext:build          # bundle extension to extensions/browser-sync/dist (esbuild)
npm run test:chrome        # build extension + Playwright tests against a mock host
npm run chrome:sandbox     # interactive Chromium with extension + fixture pages, prints messages
npm run mock-host          # stand-in for the app's dev WebSocket on 47615 (set chrome.storage.local {port: 47615} in the extension's worker)
npm run typecheck          # extension + desktop app
```

Single Chrome test: `cd tests/e2e-chrome; npx playwright test -g "no drift"`.

Desktop app (`apps/desktop`):

```powershell
npm run windows            # debug build + deploy + launch (needs Metro; it starts one)
npm start                  # Metro only (debug builds load JS from it)
npm run windows:release    # release build, JS bundled
npm test                   # Jest (@rnx-kit/jest-preset 'windows'); single: npx jest -t "renders"
npx tsc --noEmit
```

Build without launching: `npx @react-native-community/cli run-windows --no-launch --no-packager --logging --no-telemetry`. Launch the deployed package with `Start-Process "shell:AppsFolder\ScrollPen_5j6dqts9n04aa!App"`.

Engine library alone (fastest compile loop):
```powershell
$msbuild = "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
& $msbuild apps\desktop\windows\ScrollPenEngine\ScrollPenEngine.vcxproj /p:Configuration=Debug /p:Platform=x64 /m /v:minimal
```

Native-messaging helper (needed for the extension to reach a locally built app; `-Chromium` also registers it for Playwright's Chromium):
```powershell
& $msbuild apps\desktop\windows\ScrollPenHost\ScrollPenHost.vcxproj /p:Configuration=Debug /p:Platform=x64   # -> apps\desktop\windows\x64\Debug\ScrollPenHost.exe
pwsh apps\desktop\windows\installer\register-native-host.ps1 -HostExe apps\desktop\windows\x64\Debug\ScrollPenHost.exe -Chromium
```
Desktop link checks against the running app: `node tests\e2e-desktop\link-check.mjs`, `node tests\e2e-desktop\native-link.mjs`.

Release installer (extension, `ScrollPenHost.exe`, signed Release MSIX, `release\ScrollPen-Setup.exe`): `pwsh apps\desktop\windows\installer\build-setup.ps1`. It signs with the non-exportable certificate "ScrollPen sideload signing" in `Cert:\CurrentUser\My` (no key file or password exists; the script header shows how to recreate it).

Toolchain: Visual Studio 2026 (v145 toolset) with Windows SDK 10.0.22621 + 26100 and MSIX packaging tools, Node 22, PowerShell 7 (`pwsh`, required by the RNW CLI), .NET 10 SDK, Developer Mode on. `rnw-dependencies.ps1` in `apps/desktop/node_modules/react-native-windows/scripts` lists the required VS components.

## Architecture

### Native engine owns everything that's latency- or state-critical
`apps/desktop/windows/ScrollPenEngine` is a C++20 static library linked into the RNW app (`windows/ScrollPen`). The engine, not JS, owns the annotation model, undo/redo, tool state, settings persistence and scroll sync. JS is only the dock/settings UI and mirrors engine state.

- **Bridge:** `windows/ScrollPen/ScrollPenModule.h` is an attributed RN module (`ScrollPenNative`). JS calls `send(jsonCommand)`; the engine replies with `scrollpenEvent` device events (`{ev:"state", ...}` is the full state snapshot). Typed on the JS side in `src/native/scrollpen.ts`; command handling is `Engine::Impl::HandleCommand` in `Engine.cpp`. Keep both in sync when adding commands.
- **Threads:** the engine runs its own message loop (`Engine.cpp`: controller window + one overlay window per monitor), separate from the RN UI thread. Low-level mouse/keyboard hooks run on a dedicated time-critical thread (`InputHooks.cpp`) whose callbacks only read atomics and `PostMessage`; a watchdog reinstalls hooks that Windows silently removed. UI Automation and optical tracking run on a worker thread inside `ScrollTracker.cpp`. Every one of these threads calls `SetThreadDpiAwarenessContext(PER_MONITOR_AWARE_V2)`; coordinates everywhere are physical virtual-screen pixels.
- **Hooks exist only while drawing** (`SetActive`). In cursor mode ScrollPen sees no input, a deliberate privacy property.
- **Click-through / scroll pass-through:** overlays are `WS_EX_LAYERED|WS_EX_NOREDIRECTIONBITMAP` and toggle `WS_EX_TRANSPARENT`. While drawing, the mouse hook swallows a wheel event over an overlay, makes overlays click-through for a short "burst", and re-injects the wheel with `SendInput` (tagged `InputHooks::kInjectedTag` so the hooks let it pass). A button press mid-burst ends the burst and is replayed onto the overlay. The configurable hold key (default CapsLock) also makes overlays click-through.
- **Rendering:** `Renderer.cpp` draws with a shared D3D11/D2D/DWrite/DComp device. Each overlay caches committed ink in a bitmap (`cacheValid`) and redraws only live items (active stroke, text caret, laser, eraser cursor) per frame. Call `InvalidateCaches()` when the model or a document viewport changes, `Invalidate()` for live-only changes.

### Model and coordinate spaces (`Model.h`)
Items live in layers: `"screen"` (screen px), `"board"` (whiteboard/blackboard page), and document layers. Document-layer items are stored in **document units** (CSS px for browsers, physical px for UIA/optical) and mapped to the screen through the layer's live `Viewport` (`screenRect`, `scrollX/Y`, `scale`). On commit, `CommitItem` converts screen points to layer space and divides widths/font sizes by `scale`. All mutations go through `Model` so undo/redo stays consistent.

### Chrome only
Lasting ink is drawn only on the page area of a Google Chrome window (`chrome.exe`); images, PDFs and documents are meant to be opened in Chrome. `Engine::Begin` refuses other strokes and shows an on-overlay notice (`LiveState::notice`): outside Chrome, or on Chrome's tab strip/toolbar (where `Resolve` finds no page). Every lasting stroke therefore belongs to a document layer; nothing persistent goes on the `"screen"` layer, which would show over other apps. Boards, laser and eraser work anywhere.

### Scroll sync (`ScrollTracker.cpp`)
At stroke start `Resolve(pt)` binds a layer to the page under the pen:
1. **Browser** (exact): `ActiveTabFor(hwnd)` picks the tab Chrome shows in that window from extension reports. The window title must start with the tab's `document.title`; size and a zoom-corrected screen-position estimate break ties (never use that estimate as a filter: it is unit-mixed under browser zoom). `PageRect` places the page at the **bottom-left of the window's visible frame**, sized by the reported `innerWidth/innerHeight × devicePixelRatio`. Do not rely on Chrome's `Chrome_RenderWidgetHostHWND` children: in real Chrome they are hidden or stale. Layer key `web:<docKey>#<scroller>`.
   - Inner scrollers: at stroke start the app sends a `probe` (app → extension through the same link, `Engine::SendToBrowser`); the page reports the vertically scrolling element under the pen, and `Refine` rebinds the stroke at commit.
   - Image tabs: the picture is reported as scroller `image` with `contentScale`; ink is anchored to it (Chrome centres and zooms it), clipped to the whole page via `Viewport::clipArea`.
2. **Fallback** for Chrome pages the extension can't see (`chrome://`, Chrome's own PDF viewer): UI Automation `ScrollPattern`, else optical (`OpticalFlow.cpp`, `PrintWindow` row matching), polled on a worker thread only for layers with ink. These bindings remember the window title and hide when another tab is shown.

`Update()` refreshes every bound layer's viewport and clips ink to the uncovered parts of its window (`UncoveredRects`). Scroll messages and worker updates present immediately (`RenderAll()` right after `Update`); waiting for an idle queue makes ink trail the page.

### Browser extension and the app link
- `packages/shared-types/src/sync-protocol.ts` defines the messages (extension → app: `hello`, `scroll`, `zoom`, `focus`, `navigate`; app → extension: `probe`, `welcome`; helper → extension: `host`; `v: 1`).
- **Transport:** Chrome native messaging. The extension (fixed ID `egihgjpnabmnoljofdkeakgplgjeidai`, from the manifest `key`; also `kExtensionId` in `LocalPeer.h` and `register-native-host.ps1`) calls `connectNative('com.scrollpen.sync')`. Chrome starts `ScrollPenHost.exe` (`windows/ScrollPenHost`, registered in HKCU by `installer/register-native-host.ps1`, copied to `%LocalAppData%\ScrollPen\NativeHost`), which relays to the app's `PipeServer` over `\\.\pipe\ScrollPen.Sync.<user SID>` (DACL: that user only; remote clients rejected; exclusive first instance). The app accepts only `ScrollPenHost.exe` in its session as its user; the helper accepts only `ScrollPen.exe` and only ScrollPen's extension origin. Never reintroduce an unauthenticated listener: loopback ports are shared by every Windows user on the PC.
- `SyncServer.cpp` (minimal RFC 6455 WebSocket) is for tests/dev only, off unless the `devWebSocket` setting is on; the extension uses it only when `chrome.storage.local.port` is set. It still binds loopback only and requires the pinned extension Origin, a loopback Host header, a TCP peer that is `chrome.exe` in our session, handshakes within 5 s, masked single-frame text messages and size limits.
- `ScrollTracker::OnBrowserMessage` schema-checks every message (`ValidBrowserMessage`) and caps tab/scroller state; `Engine` also catches JSON type errors around it.
- `extensions/browser-sync`: `src/reporter.ts` (shared by the content script and the PDF viewer) measures scroll per animation frame; the MV3 service worker (`background.ts`) adds `tabId/windowId/zoom`, holds one link (native, or a WebSocket when a test port is set) with throttled reconnects, and whenever the app becomes reachable resyncs: `hello`, `focus` for active tabs plus a `report` request to each page. `globalThis.scrollpenLink()` in the worker reports the link state.
- **PDFs** open in the extension's own pdf.js viewer (`src/pdf/`, `pdfjs-dist` bundled by `build.mjs`): the service worker redirects `.pdf` URLs (http/https/file), including tabs already open when the extension is installed or updated. Chrome's built-in viewer hides scroll from extensions. Back from the viewer shows Chrome's viewer once (`chrome.storage.session` key `pdf-native:<tabId>`). The viewer lays pages out once at load so window resizes never move content under ink.
- Protocol changes must update the TS types, the extension and `ValidBrowserMessage`/`OnBrowserMessage` together.
- PDFs render with pdf.js 6 (no scripting, `enableXfa: false`) under the extension CSP (`script-src 'self'`); keep `pdfjs-dist` current, the viewer can read every site and local files. Release builds have no source maps (`build.mjs`, maps only with `--watch`). After changing the manifest or service worker, the unpacked extension must be reloaded in `chrome://extensions` and open tabs reloaded.

### Tests
- `tests/e2e-chrome`: Playwright **bundled Chromium** (branded Chrome 137+ ignores `--load-extension`) with fixture pages served on loopback and a mock host (`support/mock-host.mjs`, WebSocket) that accepts only the pinned extension ID and answers `hello` with `welcome`, like the app's dev WebSocket. The fixture points the extension at it via `chrome.storage.local.port`, so these tests never exercise native messaging (that's `tests/e2e-desktop/native-link.mjs`). Headless Chromium downloads PDFs instead of opening them, so the PDF redirect can't be tested there.
- `tests/e2e-desktop`: helpers for driving the real app. `link-check.mjs` (malformed/flood/impostor checks through the real helper) and `native-link.mjs` (extension → helper → app in Chromium; register the helper with `-Chromium` first) need the app running but move no input. `input.ps1` synthesizes real mouse/keyboard input (`move`, `down`, `up`, `drag`, `wheel`, `keys ctrl+minus`...) and `chromium-host.mjs` opens Chromium with the extension against the real app. These move the user's actual cursor: ask first, keep runs short, and stop if foreign strokes appear (the user often tests at the same time). Bring the test window to the front before each input batch; the IDE tends to take focus back.
- Verifying sync: measure the ink's row offset from fixture marker lines (red) in screenshots across scroll/zoom; a constant offset (or one scaling with zoom) means the ink is attached.
- Testing PDFs/images via `file://` in Playwright needs extension file access: call `chrome.developerPrivate.updateExtensionConfiguration({extensionId, fileAccess: true})` from a `chrome://extensions` page in a persistent profile, close, relaunch. Toggling it in a live session reloads and breaks a command-line-loaded extension. On this machine Internet Download Manager intercepts PDFs served over http, so use `file://`.

## Gotchas

- The app links the **static CRT** (`/MT`, `/MTd`); the engine project must match, or linking fails with `RuntimeLibrary` mismatches. New Win32 APIs may need libs added to the app's `AdditionalDependencies` in `ScrollPen.vcxproj`. Control Flow Guard is Release-only (it conflicts with Debug `/ZI`).
- The dock is hidden from screen capture by default (`hideDockFromCapture` → `WDA_EXCLUDEFROMCAPTURE`), so screenshots won't show it. Set `{"hideDockFromCapture": false}` in `%AppData%\ScrollPen\settings.json` while testing, and restore it afterwards.
- The editor's clangd has no compile database for the vcxproj and reports spurious errors (`std::optional` unknown, `HWND` unknown). Trust MSBuild output.
- The dock is a frameless WinAppSDK `AppWindow` forced to `WS_POPUP` (overlapped windows can't be narrower than `SM_CXMIN`), with a `WM_NCCALCSIZE` subclass because WinAppSDK keeps re-adding `WS_DLGFRAME` (a white 3-D border). RN `RN_TOOLTIP` popups are made click-through (WinEvent hook in `ScrollPen.cpp`) because lingering tooltips covered dock buttons. Dock dragging is a native cursor-polling thread (`ScrollPenModule.h`), not `WM_NCLBUTTONDOWN`.
- The app is single-instance per login session (`Local\ScrollPen.SingleInstance` mutex). Stop the running `ScrollPen` process before redeploying.
- `apps/desktop/patches/braces` is braces 3.0.3 with a nesting-depth limit (GHSA-vfj7-8cjw-p6xm had no fixed release); `package.json` overrides it, plus `@xmldom/xmldom`, `fast-xml-parser` and the istanbul `js-yaml`. Drop overrides once upstream fixes ship; `npm audit` must stay at 0 in both roots.
- In the Bash tool `python3` may resolve to the Windows Store stub; use `python`.
- Git: `.gitattributes` forces LF except VS project/solution files (CRLF). Commit messages written from PowerShell should go through a UTF-8 file **without BOM** (`git commit -F file`).
