# ScrollPen

A screen pen for Windows whose ink can stay attached to the page while you scroll.

Windows 10/11 only. All drawing is rendered locally on your GPU (Direct2D / DirectComposition); nothing is sent to a cloud service.

> Status: Phase 0 (technical validation). Not usable yet.

## Repository layout

```
apps/desktop/            React Native for Windows 0.84 app (TypeScript UI + C++ host)
  windows/               Visual Studio solution (ScrollPen.sln)
extensions/browser-sync/ Chrome/Edge extension that reports page scroll to the app
packages/shared-types/   Message types shared by the app and the extension
tests/e2e-chrome/        Playwright tests + fixture pages + mock ScrollPen host
```

## Development setup (Windows 11)

1. Visual Studio 2026 with: Desktop development with C++, .NET desktop development, WinUI application development, Windows 11 SDK 10.0.22621 and 10.0.26100.
   Fastest route: run `apps\desktop\node_modules\react-native-windows\scripts\rnw-dependencies.ps1` in an elevated PowerShell after step 3.
2. Node.js 22 LTS, PowerShell 7 (`pwsh`), .NET 10 SDK, Git. Developer Mode on.
3. Install dependencies:
   ```powershell
   npm install              # extension, shared types, Chrome tests
   npm run app:install      # the desktop app
   npx playwright install chromium
   ```

## Run the app

```powershell
npm run app:windows      # debug build + launch (starts Metro)
```

## Testing in Chrome

The extension is what makes ink follow scrolling in browsers, so it has its own test suite.

**Automated** (`npm run test:chrome`): builds the extension, serves the pages in `tests/e2e-chrome/fixtures` on 127.0.0.1, launches Playwright's Chromium with the extension loaded, and checks every message against a mock ScrollPen host. Branded Google Chrome 137+ ignores `--load-extension`, so automated runs use Playwright's Chromium (same engine).

**Interactive sandbox** (`npm run chrome:sandbox`): opens a Chromium window with the extension and fixture pages loaded; every message the app would receive prints in the terminal. Scroll, zoom and switch tabs to see it live.

**Your own Chrome or Edge** (manual check before a release):
1. `npm run ext:build`, then `npm run mock-host` (listens on the app's real port, 47615).
2. Start a separate profile so your everyday one is untouched:
   `& "C:\Program Files\Google\Chrome\Application\chrome.exe" --user-data-dir="$env:TEMP\scrollpen-chrome"`
3. Open `chrome://extensions`, turn on Developer mode, click **Load unpacked**, pick `extensions/browser-sync/dist`.
4. Browse any page; messages appear in the mock-host terminal.

Known limit: Chrome's built-in PDF viewer doesn't run extensions, so PDFs in Chrome fall back to the optical tracker.

## Privacy

The extension talks only to `ws://127.0.0.1`, and the app's sync port accepts only browser extensions, so ordinary web pages can't connect to it (covered by a test). Page URLs never leave the machine.
