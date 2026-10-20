#include "pch.h"
#include "InputHooks.h"

namespace sp {

InputHooks *InputHooks::instance_ = nullptr;

namespace {
constexpr UINT_PTR kWatchdogTimer = 1;
constexpr UINT kWatchdogMs = 1000;
// Input the OS saw this long after the hooks last fired means the hooks are gone.
constexpr DWORD kSilenceMs = 1500;
} // namespace

bool InputHooks::Start(Callbacks callbacks) {
  if (thread_.joinable()) return true;
  callbacks_ = std::move(callbacks);
  instance_ = this;
  ready_ = false;
  thread_ = std::thread([this] { ThreadMain(); });
  while (!ready_) std::this_thread::yield();
  return mouse_ && key_;
}

void InputHooks::Stop() {
  if (!thread_.joinable()) return;
  PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
  thread_.join();
  instance_ = nullptr;
}

bool InputHooks::Install() {
  const HINSTANCE mod = GetModuleHandleW(nullptr);
  mouse_ = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, mod, 0);
  key_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, mod, 0);
  lastSeenTick_ = GetTickCount();
  return mouse_ && key_;
}

void InputHooks::Uninstall() {
  if (mouse_) UnhookWindowsHookEx(mouse_);
  if (key_) UnhookWindowsHookEx(key_);
  mouse_ = key_ = nullptr;
}

void InputHooks::ThreadMain() {
  threadId_ = GetCurrentThreadId();
  SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // Hook callbacks run on this thread; keep it ahead of normal work so we never time out.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  MSG msg;
  PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE); // create the queue
  Install();
  ready_ = true;
  SetTimer(nullptr, kWatchdogTimer, kWatchdogMs, nullptr);

  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_TIMER) {
      LASTINPUTINFO li{sizeof(li)};
      if (GetLastInputInfo(&li) && static_cast<LONG>(li.dwTime - lastSeenTick_.load()) > static_cast<LONG>(kSilenceMs)) {
        Uninstall();
        Install();
        ++reinstalls_;
        if (callbacks_.onReinstalled) callbacks_.onReinstalled();
      }
      continue;
    }
    DispatchMessageW(&msg);
  }
  Uninstall();
}

LRESULT CALLBACK InputHooks::MouseProc(int code, WPARAM wp, LPARAM lp) {
  auto *self = instance_;
  if (code == HC_ACTION && self) {
    self->lastSeenTick_ = GetTickCount();
    const auto &info = *reinterpret_cast<MSLLHOOKSTRUCT *>(lp);
    if (info.dwExtraInfo != kInjectedTag && self->callbacks_.onMouse && self->callbacks_.onMouse(wp, info)) return 1;
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK InputHooks::KeyProc(int code, WPARAM wp, LPARAM lp) {
  auto *self = instance_;
  if (code == HC_ACTION && self) {
    self->lastSeenTick_ = GetTickCount();
    const auto &info = *reinterpret_cast<KBDLLHOOKSTRUCT *>(lp);
    if (info.dwExtraInfo != kInjectedTag && self->callbacks_.onKey && self->callbacks_.onKey(wp, info)) return 1;
  }
  return CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace sp
