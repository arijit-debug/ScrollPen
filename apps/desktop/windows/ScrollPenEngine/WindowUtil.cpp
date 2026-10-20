#include "pch.h"
#include "WindowUtil.h"

namespace sp {

std::vector<MonitorInfo> EnumerateMonitors() {
  std::vector<MonitorInfo> out;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO mi{sizeof(mi)};
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        MonitorInfo info;
        info.handle = m;
        info.rect = mi.rcMonitor;
        info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) info.dpi = dx;
        reinterpret_cast<std::vector<MonitorInfo> *>(lp)->push_back(info);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  return out;
}

bool IsOwnWindow(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

namespace {

bool IsCloaked(HWND hwnd) {
  DWORD cloaked = 0;
  return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

} // namespace

RECT VisibleFrame(HWND hwnd) {
  RECT r{};
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(hwnd, &r);
  return r;
}

HWND TopLevelWindowAt(POINT pt) {
  for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
    if (!IsWindowVisible(w) || IsIconic(w) || IsCloaked(w) || IsOwnWindow(w)) continue;
    const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
    // Other apps' click-through overlays (e.g. other screen pens, game overlays).
    if ((ex & WS_EX_TRANSPARENT) && (ex & WS_EX_LAYERED)) continue;
    const RECT r = VisibleFrame(w);
    if (PtInRect(&r, pt)) return w;
  }
  return nullptr;
}

std::wstring ProcessName(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return {};
  wchar_t buf[MAX_PATH];
  DWORD n = MAX_PATH;
  std::wstring name;
  if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
    name.assign(buf, n);
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
  }
  CloseHandle(p);
  return name;
}

std::wstring WindowClass(HWND hwnd) {
  wchar_t buf[256]{};
  GetClassNameW(hwnd, buf, 256);
  return buf;
}

std::wstring WindowTitle(HWND hwnd) {
  wchar_t buf[512]{};
  GetWindowTextW(hwnd, buf, 512);
  return buf;
}

bool IsChromiumBrowser(HWND topLevel) {
  if (WindowClass(topLevel) != L"Chrome_WidgetWin_1") return false;
  const std::wstring p = ProcessName(topLevel);
  return p == L"chrome.exe" || p == L"msedge.exe" || p == L"brave.exe" || p == L"vivaldi.exe" || p == L"opera.exe" ||
      p == L"chromium.exe";
}

std::optional<RECT> ChromiumViewport(HWND topLevel) {
  struct Ctx {
    RECT best{};
    LONG bestArea{0};
  } ctx;
  EnumChildWindows(
      topLevel,
      [](HWND child, LPARAM lp) -> BOOL {
        if (!IsWindowVisible(child)) return TRUE;
        if (WindowClass(child) != L"Chrome_RenderWidgetHostHWND") return TRUE;
        RECT r{};
        GetWindowRect(child, &r);
        auto *c = reinterpret_cast<Ctx *>(lp);
        const LONG area = (r.right - r.left) * (r.bottom - r.top);
        // The page is the largest render widget (others are popups, devtools, side panels).
        if (area > c->bestArea) {
          c->bestArea = area;
          c->best = r;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  if (ctx.bestArea == 0) return std::nullopt;
  return ctx.best;
}

bool IsElevated(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) return true; // can't even query: treat as more privileged than us
  HANDLE token = nullptr;
  bool elevated = false;
  if (OpenProcessToken(p, TOKEN_QUERY, &token)) {
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    if (GetTokenInformation(token, TokenElevation, &e, sizeof(e), &n)) elevated = e.TokenIsElevated != 0;
    CloseHandle(token);
  }
  CloseHandle(p);
  return elevated;
}

std::string Narrow(const std::wstring &w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Widen(const std::string &s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

} // namespace sp
