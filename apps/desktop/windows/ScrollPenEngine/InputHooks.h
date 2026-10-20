#pragma once

#include <atomic>
#include <functional>
#include <thread>

namespace sp {

// Low-level mouse + keyboard hooks on a dedicated thread.
//
// Windows silently removes a low-level hook whose callback exceeds LowLevelHooksTimeout,
// so callbacks must only read atomics and post messages. A watchdog compares
// GetLastInputInfo with the last event the hooks saw and reinstalls them if they went quiet.
//
// Privacy: events are inspected in memory to decide "swallow or pass" and are never
// stored, logged or sent anywhere.
class InputHooks {
 public:
  // dwExtraInfo tag on input we inject ourselves, so the hooks let it straight through.
  static constexpr ULONG_PTR kInjectedTag = 0x5C501F00;

  struct Callbacks {
    // Return true to swallow the event. Called on the hook thread.
    std::function<bool(WPARAM msg, const MSLLHOOKSTRUCT &)> onMouse;
    std::function<bool(WPARAM msg, const KBDLLHOOKSTRUCT &)> onKey;
    std::function<void()> onReinstalled;
  };

  bool Start(Callbacks callbacks);
  void Stop();
  ~InputHooks() { Stop(); }

  unsigned ReinstallCount() const { return reinstalls_.load(); }

 private:
  void ThreadMain();
  bool Install();
  void Uninstall();
  static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
  static LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp);

  Callbacks callbacks_;
  std::thread thread_;
  std::atomic<DWORD> threadId_{0};
  std::atomic<bool> ready_{false};
  HHOOK mouse_{};
  HHOOK key_{};
  std::atomic<DWORD> lastSeenTick_{0};
  std::atomic<unsigned> reinstalls_{0};
  static InputHooks *instance_;
};

} // namespace sp
