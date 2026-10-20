#pragma once

// Bridge between the React Native dock (JS) and the native engine.
// JS -> native: send(json command). Native -> JS: "scrollpenEvent" device events (json).

#include <algorithm>
#include <atomic>
#include <thread>

#include "NativeModules.h"

#include "../ScrollPenEngine/Engine.h"

namespace ScrollPen {

// Set by WinMain before the React instance starts.
inline HWND g_dockHwnd = nullptr;

inline std::atomic<bool> g_dockDragging{false};

// Moves the dock with the cursor until the mouse button is released. Runs on its own
// thread and polls the cursor, so it works no matter which window holds mouse capture or
// whether the button was already released by the time JS asked (a quick click).
inline void DragDockUntilRelease() {
  if (!g_dockHwnd || g_dockDragging.exchange(true)) return;
  std::thread([] {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // GetAsyncKeyState reads physical buttons; the primary one is the right when swapped.
    const int button = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    HANDLE tick = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    POINT start{}, last{};
    RECT origin{};
    GetCursorPos(&start);
    GetWindowRect(g_dockHwnd, &origin);
    last = start;
    while (GetAsyncKeyState(button) & 0x8000) {
      POINT p{};
      GetCursorPos(&p);
      if (p.x != last.x || p.y != last.y) {
        last = p;
        RECT now{};
        GetWindowRect(g_dockHwnd, &now);
        const int w = now.right - now.left, h = now.bottom - now.top;
        // Keep the whole dock inside the work area of the monitor under the cursor.
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
        const RECT &wa = mi.rcWork;
        const int x = std::clamp(static_cast<int>(origin.left + p.x - start.x), static_cast<int>(wa.left),
                                 std::max(static_cast<int>(wa.left), static_cast<int>(wa.right) - w));
        const int y = std::clamp(static_cast<int>(origin.top + p.y - start.y), static_cast<int>(wa.top),
                                 std::max(static_cast<int>(wa.top), static_cast<int>(wa.bottom) - h));
        SetWindowPos(g_dockHwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
      }
      LARGE_INTEGER due{};
      due.QuadPart = -40000; // 4 ms
      if (tick && SetWaitableTimer(tick, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(tick, 50);
      else Sleep(4);
    }
    if (tick) CloseHandle(tick);
    g_dockDragging = false;
  }).detach();
}

REACT_MODULE(ScrollPenNative)
struct ScrollPenNative {
  REACT_INIT(Initialize)
  void Initialize(winrt::Microsoft::ReactNative::ReactContext const &context) noexcept {
    context_ = context;
    auto self = this;
    sp::Engine::Instance().Start(g_dockHwnd, [self](const std::string &json) {
      if (self->OnEvent) self->OnEvent(json);
    });
  }

  REACT_METHOD(Send, L"send")
  void Send(std::string json) noexcept {
    sp::Engine::Instance().Post(json);
  }

  // Lets the user drag the frameless dock by its grip.
  REACT_METHOD(StartDockDrag, L"startDockDrag")
  void StartDockDrag() noexcept {
    DragDockUntilRelease();
  }

  // Resizes the dock window (logical px; e.g. when the settings panel opens).
  REACT_METHOD(ResizeDock, L"resizeDock")
  void ResizeDock(double width, double height) noexcept {
    context_.UIDispatcher().Post([width, height] {
      if (!g_dockHwnd) return;
      const UINT dpi = GetDpiForWindow(g_dockHwnd);
      const int w = MulDiv(static_cast<int>(width), dpi, 96), h = MulDiv(static_cast<int>(height), dpi, 96);
      // Growing (palette, settings) must not push the dock off its monitor's work area.
      RECT r{};
      GetWindowRect(g_dockHwnd, &r);
      MONITORINFO mi{sizeof(mi)};
      GetMonitorInfoW(MonitorFromWindow(g_dockHwnd, MONITOR_DEFAULTTONEAREST), &mi);
      const RECT &wa = mi.rcWork;
      const int x = std::max(static_cast<int>(wa.left), std::min(static_cast<int>(r.left), static_cast<int>(wa.right) - w));
      const int y = std::max(static_cast<int>(wa.top), std::min(static_cast<int>(r.top), static_cast<int>(wa.bottom) - h));
      SetWindowPos(g_dockHwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    });
  }

  REACT_METHOD(Quit, L"quit")
  void Quit() noexcept {
    context_.UIDispatcher().Post([] {
      sp::Engine::Instance().Stop();
      if (g_dockHwnd) PostMessageW(g_dockHwnd, WM_CLOSE, 0, 0);
    });
  }

  REACT_METHOD(MinimizeDock, L"minimizeDock")
  void MinimizeDock() noexcept {
    context_.UIDispatcher().Post([] {
      if (g_dockHwnd) ShowWindow(g_dockHwnd, SW_MINIMIZE);
    });
  }

  REACT_EVENT(OnEvent, L"scrollpenEvent")
  std::function<void(std::string)> OnEvent;

 private:
  winrt::Microsoft::ReactNative::ReactContext context_{nullptr};
};

} // namespace ScrollPen
