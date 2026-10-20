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

  // Refreshes viewports of all bound layers. Returns true if anything visible changed.
  bool Update(Model &model);

  // Wheel/keyboard/drag activity near pt: track at full rate for a short while.
  void NoteActivity(POINT pt);

  // Manual correction for the layer under pt (Ctrl+Up/Down). Returns false if none.
  bool Nudge(Model &model, POINT pt, double dyPhysical);

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
    double lastSeen{};
    struct Scroller {
      double x{}, y{};
      RectF rectCss; // relative to the page viewport
    };
    std::map<std::string, Scroller> scrollers; // "document" or CSS path
  };

  struct Binding {
    SyncSource source{SyncSource::None};
    HWND hwnd{};
    // Browser
    std::string docKey;
    std::string scroller;
    // UIA / optical: shared with the worker by index.
    int workerSlot{-1};
    double manualDy{}; // document units
  };

  struct WorkerSlot; // defined in .cpp
  class Worker;

  std::optional<Target> ResolveBrowser(HWND hwnd, POINT pt);
  std::optional<int> ActiveTabFor(HWND hwnd, const RECT &viewport);
  bool UpdateBrowserLayer(Layer &layer, Binding &b);

  std::map<int, TabState> tabs_;
  std::map<int, int> activeTabByWindow_; // chrome windowId -> tabId
  std::map<std::string, Binding> bindings_; // layer key -> binding
  std::unique_ptr<Worker> worker_;
  std::function<void()> wake_;
  SyncSource activeSource_{SyncSource::None};
  float activeConfidence_{0};
};

} // namespace sp
