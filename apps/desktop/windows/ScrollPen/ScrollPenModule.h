#pragma once

// Bridge between the React Native dock (JS) and the native engine.
// JS -> native: send(json command). Native -> JS: "scrollpenEvent" device events (json).

#include "NativeModules.h"

#include "../ScrollPenEngine/Engine.h"

namespace ScrollPen {

// Set by WinMain before the React instance starts.
inline HWND g_dockHwnd = nullptr;

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
    context_.UIDispatcher().Post([] {
      if (!g_dockHwnd) return;
      ReleaseCapture();
      SendMessageW(g_dockHwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    });
  }

  // Resizes the dock window (logical px; e.g. when the settings panel opens).
  REACT_METHOD(ResizeDock, L"resizeDock")
  void ResizeDock(double width, double height) noexcept {
    context_.UIDispatcher().Post([width, height] {
      if (!g_dockHwnd) return;
      const UINT dpi = GetDpiForWindow(g_dockHwnd);
      const int w = MulDiv(static_cast<int>(width), dpi, 96), h = MulDiv(static_cast<int>(height), dpi, 96);
      SetWindowPos(g_dockHwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
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
