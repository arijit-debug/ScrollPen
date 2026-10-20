#pragma once

#include "Model.h"

namespace sp {

enum class SyncSource : uint8_t { None, Browser, Uia, Optical };
const char *SyncSourceName(SyncSource s);

// Finds which document is under a point and keeps every document layer's Viewport
// (screen rect + scroll offset) up to date.
//
// Browser data arrives from the extension; UI Automation and optical tracking run on a
// worker thread so a slow or hung target app can never stall ink rendering.
class ScrollTracker {
 public:
  ScrollTracker();
  ~ScrollTracker();

  // Called when the worker has new data and a redraw is needed.
  void SetWakeCallback(std::function<void()> wake) { wake_ = std::move(wake); }
  // Sends a message to the browser extension (probes for the scroller under the pen).
  void SetSendCallback(std::function<void(const std::string &)> send) { send_ = std::move(send); }

  // A JSON message from the browser extension (already parsed).
  void OnBrowserMessage(const nlohmann::json &msg);

  struct Target {
    std::string layerKey;
    Viewport viewport;
    SyncSource source{SyncSource::None};
    HWND hwnd{};
  };
  // Resolves the document under a screen point and binds a layer to it. Blocks for at
  // most ~150 ms on UI Automation.
  std::optional<Target> Resolve(POINT pt);

  // Called when the stroke is committed. The page answers Resolve's probe within a few
  // milliseconds; if the stroke started inside an inner scrolling element (a chat pane,
  // an editor), this returns a target bound to that element instead of the document.
  std::optional<Target> Refine(const Target &target, POINT pt);

  // Refreshes viewports of all bound layers. Returns true if anything visible changed.
  bool Update(Model &model);

  // Wheel/keyboard/drag activity near pt: track at full rate for a short while.
  void NoteActivity(POINT pt);

  // Manual correction for the layer under pt (Ctrl+Up/Down). Returns false if none.
  bool Nudge(Model &model, POINT pt, double dyPhysical);

  // True when pt is over a Google Chrome window, the only place ScrollPen draws.
  bool IsChromeAt(POINT pt);

  // Info for the dock's sync indicator: the best source currently visible.
  SyncSource ActiveSource() const { return activeSource_; }
  float ActiveConfidence() const { return activeConfidence_; }

 private:
  struct TabState {
    int windowId{-1};
    std::string docKey;
    double zoom{1};
    double dpr{1};
    double viewportW{}, viewportH{};
    double screenX{}, screenY{}; // viewport origin in DIPs (approximate)
    std::wstring title;          // the page's title; Chrome's window title starts with it
    double lastSeen{};
    struct Scroller {
      double x{}, y{};
      RectF rectCss; // relative to the page viewport
      // Image tabs: CSS px per image px (Chrome shrinks large images to fit the window).
      double contentScale{1};
    };
    std::map<std::string, Scroller> scrollers; // "document" or CSS path
  };

  struct Binding {
    SyncSource source{SyncSource::None};
    HWND hwnd{};
    // Browser
    std::string docKey;
    std::string scroller;
    int tabId{-1};       // last tab matched to this window
    double lastMatch{};  // when that match was made (steady seconds)
    // UIA / optical: shared with the worker by index.
    int workerSlot{-1};
    // UIA / optical in Chrome: the window title (= active tab's title) when the ink was
    // drawn; the ink shows only while that tab is.
    std::wstring title;
    double manualDy{}; // document units
  };

  struct WorkerSlot; // defined in .cpp
  class Worker;

  std::optional<Target> ResolveBrowser(HWND hwnd, POINT pt, int tabId, bool probe);
  // Where tab t's page is on screen inside Chrome window hwnd.
  static std::optional<RECT> PageRect(HWND hwnd, const TabState &t);
  // The tab shown in Chrome window hwnd, if its page reports to us.
  std::optional<int> ActiveTabFor(HWND hwnd);
  static std::wstring CurrentTitle(HWND hwnd);
  bool UpdateBrowserLayer(Layer &layer, Binding &b);

  std::map<int, TabState> tabs_;
  std::map<int, int> activeTabByWindow_; // chrome windowId -> tabId
  std::map<std::string, Binding> bindings_; // layer key -> binding
  std::unique_ptr<Worker> worker_;
  std::function<void()> wake_;
  std::function<void(const std::string &)> send_;
  SyncSource activeSource_{SyncSource::None};
  float activeConfidence_{0};
};

} // namespace sp
