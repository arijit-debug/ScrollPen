#pragma once

#include "Model.h"

namespace sp {

struct MonitorInfo {
  HMONITOR handle{};
  RECT rect{}; // physical px, virtual-screen coordinates
  UINT dpi{96};
  bool primary{false};
};

std::vector<MonitorInfo> EnumerateMonitors();

// True for windows created by this process (overlays, dock) so hit-testing can skip them.
bool IsOwnWindow(HWND hwnd);

// Topmost visible, non-cloaked top-level window at pt that isn't ours (z-order walk,
// unaffected by whether our overlay is currently click-through).
HWND TopLevelWindowAt(POINT pt);

// Executable name in lower case, e.g. L"chrome.exe". Empty if unavailable (e.g. elevated).
std::wstring ProcessName(HWND hwnd);
std::wstring WindowClass(HWND hwnd);
std::wstring WindowTitle(HWND hwnd);

bool IsChromiumBrowser(HWND topLevel);

// Chromium's content area (Chrome_RenderWidgetHostHWND) inside a browser window.
std::optional<RECT> ChromiumViewport(HWND topLevel);

// Window frame rect without the invisible resize borders.
RECT VisibleFrame(HWND hwnd);

bool IsElevated(HWND hwnd);

inline RectF ToRectF(const RECT &r) {
  return {static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
          static_cast<float>(r.bottom)};
}

std::string Narrow(const std::wstring &w);
std::wstring Widen(const std::string &s);

} // namespace sp
