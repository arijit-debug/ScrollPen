#include "pch.h"
#include "ScrollTracker.h"
#include "OpticalFlow.h"
#include "WindowUtil.h"

#pragma comment(lib, "uiautomationcore.lib")

namespace sp {

using Microsoft::WRL::ComPtr;
using json = nlohmann::json;

const char *SyncSourceName(SyncSource s) {
  switch (s) {
    case SyncSource::Browser:
      return "browser";
    case SyncSource::Uia:
      return "uia";
    case SyncSource::Optical:
      return "optical";
    default:
      return "none";
  }
}

namespace {

double Now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string Hex(uintptr_t v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
  return buf;
}

constexpr double kActiveWindowSeconds = 0.8; // track at full rate this long after activity

} // namespace

// ---------------------------------------------------------------------------------------
// Worker: UI Automation + optical tracking off the engine thread.

struct ScrollTracker::WorkerSlot {
  SyncSource source{SyncSource::None};
  HWND hwnd{}; // top-level window
  HWND captureHwnd{}; // optical: the child window whose content we track
  ComPtr<IUIAutomationElement> element;
  OpticalFlow flow;
  // Results, guarded by Worker::mutex_.
  RectF screenRect;
  double scrollX{}, scrollY{};
  float confidence{0};
  bool alive{true};
  uint64_t version{0};
};

class ScrollTracker::Worker {
 public:
  explicit Worker(std::function<void()> wake) : wake_(std::move(wake)) {
    thread_ = std::thread([this] { Run(); });
  }
  ~Worker() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
  }

  void SetWake(std::function<void()> wake) {
    std::lock_guard lock(mutex_);
    wake_ = std::move(wake);
  }

  // Synchronously binds a slot for the scrollable content at pt. -1 if nothing usable.
  int Bind(POINT pt, HWND hwnd, std::string *keyOut) {
    std::unique_lock lock(mutex_);
    // Shared so a timed-out request stays valid while the worker finishes it.
    auto req = std::make_shared<Request>(Request{pt, hwnd});
    requests_.push_back(req);
    cv_.notify_all();
    cv_.wait_for(lock, std::chrono::milliseconds(400), [&] { return req->done; });
    if (!req->done) {
      requests_.erase(std::remove(requests_.begin(), requests_.end(), req), requests_.end());
      return -1;
    }
    *keyOut = req->key;
    return req->slot;
  }

  void Activity() {
    lastActivity_ = Now();
    cv_.notify_all();
  }

  struct Snapshot {
    SyncSource source;
    HWND hwnd;
    RectF screenRect;
    double scrollX, scrollY;
    float confidence;
    bool alive;
    uint64_t version;
  };
  std::optional<Snapshot> Get(int slot) {
    std::lock_guard lock(mutex_);
    if (slot < 0 || slot >= static_cast<int>(slots_.size())) return std::nullopt;
    const auto &s = *slots_[slot];
    return Snapshot{s.source, s.hwnd, s.screenRect, s.scrollX, s.scrollY, s.confidence, s.alive, s.version};
  }

  void AddManual(int slot, double dy) {
    std::lock_guard lock(mutex_);
    if (slot >= 0 && slot < static_cast<int>(slots_.size())) slots_[slot]->scrollY += dy;
  }

 private:
  struct Request {
    POINT pt;
    HWND hwnd;
    bool done{false};
    int slot{-1};
    std::string key;
  };

  void Run() {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IUIAutomation> uia;
    if (SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia))) ||
        SUCCEEDED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia)))) {
      ComPtr<IUIAutomation2> uia2;
      // A hung target app must not hang us: cap every UIA call.
      if (SUCCEEDED(uia.As(&uia2))) {
        uia2->put_ConnectionTimeout(300);
        uia2->put_TransactionTimeout(200);
      }
    }
    uia_ = uia;

    std::unique_lock lock(mutex_);
    while (!stop_) {
      if (!requests_.empty()) {
        auto req = requests_.front();
        requests_.erase(requests_.begin());
        lock.unlock();
        std::string key;
        auto slot = MakeSlot(req->pt, req->hwnd, &key);
        lock.lock();
        if (slot) {
          slots_.push_back(std::move(slot));
          req->slot = static_cast<int>(slots_.size()) - 1;
          req->key = key;
        }
        req->done = true;
        cv_.notify_all();
        continue;
      }

      const bool active = Now() - lastActivity_ < kActiveWindowSeconds;
      // Poll every live slot. 30 Hz while the user is scrolling, 2 Hz otherwise.
      std::vector<WorkerSlot *> live;
      for (auto &s : slots_)
        if (s->alive) live.push_back(s.get());
      lock.unlock();
      bool changed = false;
      for (auto *s : live) changed |= Poll(*s, active);
      lock.lock();
      if (changed && wake_) {
        auto w = wake_;
        lock.unlock();
        w();
        lock.lock();
      }
      cv_.wait_for(lock, std::chrono::milliseconds(active ? 33 : 500),
                   [&] { return stop_ || !requests_.empty(); });
    }
    lock.unlock();
    slots_.clear();
    uia_.Reset();
    CoUninitialize();
  }

  std::unique_ptr<WorkerSlot> MakeSlot(POINT pt, HWND hwnd, std::string *key) {
    auto slot = std::make_unique<WorkerSlot>();
    slot->hwnd = hwnd;
    const std::string proc = Narrow(ProcessName(hwnd));

    // 1) UI Automation scroll pattern on the element under the point (or an ancestor).
    if (uia_) {
      ComPtr<IUIAutomationElement> el;
      ComPtr<IUIAutomationTreeWalker> walker;
      uia_->get_RawViewWalker(&walker);
      if (SUCCEEDED(uia_->ElementFromPoint(pt, &el)) && el && walker) {
        for (int depth = 0; el && depth < 30; ++depth) {
          ComPtr<IUIAutomationScrollPattern> sp;
          BOOL vScroll = FALSE;
          if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp))) && sp &&
              SUCCEEDED(sp->get_CurrentVerticallyScrollable(&vScroll)) && vScroll) {
            slot->source = SyncSource::Uia;
            slot->element = el;
            SAFEARRAY *rid = nullptr;
            std::string ridText;
            if (SUCCEEDED(el->GetRuntimeId(&rid)) && rid) {
              LONG lo = 0, hi = -1;
              SafeArrayGetLBound(rid, 1, &lo);
              SafeArrayGetUBound(rid, 1, &hi);
              for (LONG i = lo; i <= hi; ++i) {
                int v = 0;
                SafeArrayGetElement(rid, &i, &v);
                ridText += Hex(static_cast<uint32_t>(v)) + ".";
              }
              SafeArrayDestroy(rid);
            }
            *key = "uia:" + proc + ":" + Hex(reinterpret_cast<uintptr_t>(hwnd)) + ":" + ridText;
            PollUia(*slot);
            if (slot->confidence > 0) return slot;
            break;
          }
          ComPtr<IUIAutomationElement> parent;
          if (FAILED(walker->GetParentElement(el.Get(), &parent))) break;
          el = parent;
        }
      }
    }

    // 2) Optical tracking of the child window under the point.
    slot->element.Reset();
    slot->source = SyncSource::Optical;
    HWND child = hwnd;
    for (;;) {
      POINT cp = pt;
      ScreenToClient(child, &cp);
      HWND next = RealChildWindowFromPoint(child, cp);
      if (!next || next == child) break;
      child = next;
    }
    slot->captureHwnd = child;
    RECT cr{};
    GetWindowRect(child, &cr);
    slot->screenRect = ToRectF(cr);
    if (!slot->flow.Prime(hwnd, cr)) return nullptr; // window can't be captured
    *key = "opt:" + proc + ":" + Hex(reinterpret_cast<uintptr_t>(hwnd)) + ":" + Hex(reinterpret_cast<uintptr_t>(child));
    slot->confidence = 0.5f;
    return slot;
  }

  bool PollUia(WorkerSlot &s) {
    ComPtr<IUIAutomationScrollPattern> sp;
    RECT r{};
    double vPct = 0, vSize = 100, hPct = 0, hSize = 100;
    if (FAILED(s.element->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&sp))) || !sp ||
        FAILED(s.element->get_CurrentBoundingRectangle(&r)) || FAILED(sp->get_CurrentVerticalScrollPercent(&vPct)) ||
        FAILED(sp->get_CurrentVerticalViewSize(&vSize))) {
      std::lock_guard lock(mutex_);
      s.alive = IsWindow(s.hwnd) != FALSE;
      s.confidence = 0;
      return false;
    }
    sp->get_CurrentHorizontalScrollPercent(&hPct);
    sp->get_CurrentHorizontalViewSize(&hSize);
    const double vh = r.bottom - r.top, vw = r.right - r.left;
    auto offset = [](double pct, double viewSizePct, double viewPx) {
      if (pct < 0 || viewSizePct <= 0 || viewSizePct >= 100) return 0.0; // UIA_ScrollPatternNoScroll
      const double docPx = viewPx * 100.0 / viewSizePct;
      return pct / 100.0 * (docPx - viewPx);
    };
    const double y = offset(vPct, vSize, vh), x = offset(hPct, hSize, vw);
    std::lock_guard lock(mutex_);
    const RectF rect = ToRectF(r);
    const bool changed = std::abs(y - s.scrollY) > 0.25 || std::abs(x - s.scrollX) > 0.25 || rect.left != s.screenRect.left ||
        rect.top != s.screenRect.top || rect.right != s.screenRect.right || rect.bottom != s.screenRect.bottom;
    s.scrollX = x;
    s.scrollY = y;
    s.screenRect = rect;
    // Percent-based positions lose precision on very long documents.
    s.confidence = vSize > 2 ? 0.9f : 0.6f;
    if (changed) ++s.version;
    return changed;
  }

  bool PollOptical(WorkerSlot &s, bool active) {
    if (!active) return false; // optical only runs while the user is scrolling
    if (!IsWindow(s.captureHwnd)) {
      std::lock_guard lock(mutex_);
      s.alive = false;
      return false;
    }
    RECT cr{};
    GetWindowRect(s.captureHwnd, &cr);
    const auto m = s.flow.Measure(s.hwnd, cr);
    std::lock_guard lock(mutex_);
    const RectF rect = ToRectF(cr);
    bool changed = rect.left != s.screenRect.left || rect.top != s.screenRect.top;
    s.screenRect = rect;
    if (m.valid) {
      s.scrollY += m.dy;
      s.confidence = m.confidence;
      changed |= m.dy != 0;
    }
    if (changed) ++s.version;
    return changed;
  }

  bool Poll(WorkerSlot &s, bool active) {
    if (!IsWindow(s.hwnd)) {
      std::lock_guard lock(mutex_);
      s.alive = false;
      return true;
    }
    return s.source == SyncSource::Uia ? PollUia(s) : PollOptical(s, active);
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_{false};
  std::vector<std::shared_ptr<Request>> requests_;
  std::vector<std::unique_ptr<WorkerSlot>> slots_;
  std::atomic<double> lastActivity_{0};
  std::function<void()> wake_;
  ComPtr<IUIAutomation> uia_;
};

// ---------------------------------------------------------------------------------------

ScrollTracker::ScrollTracker() {
  worker_ = std::make_unique<Worker>([this] {
    if (wake_) wake_();
  });
}

ScrollTracker::~ScrollTracker() = default;

void ScrollTracker::OnBrowserMessage(const json &msg) {
  if (!msg.is_object() || msg.value("v", 0) != 1) return;
  const std::string type = msg.value("type", "");
  const int tabId = msg.value("tabId", -1);
  const double now = Now();

  if (type == "focus") {
    activeTabByWindow_[msg.value("windowId", -1)] = tabId;
    if (auto it = tabs_.find(tabId); it != tabs_.end()) it->second.windowId = msg.value("windowId", -1);
    return;
  }
  if (tabId < 0) return;
  if (type == "navigate") {
    if (!msg.contains("docKey") || msg["docKey"].is_null()) {
      tabs_.erase(tabId);
      return;
    }
    auto &t = tabs_[tabId];
    const std::string docKey = msg.value("docKey", "");
    if (t.docKey != docKey) t.scrollers.clear();
    t.docKey = docKey;
    t.lastSeen = now;
    return;
  }
  if (type == "zoom") {
    tabs_[tabId].zoom = msg.value("zoom", 1.0);
    return;
  }
  if (type != "scroll") return;

  auto &t = tabs_[tabId];
  t.docKey = msg.value("docKey", t.docKey);
  if (msg.contains("windowId")) {
    t.windowId = msg.value("windowId", -1);
    // The tab producing scroll events is the visible one in its window.
    if (msg.value("visible", true)) activeTabByWindow_[t.windowId] = tabId;
  }
  if (msg.contains("zoom")) t.zoom = msg.value("zoom", 1.0);
  t.dpr = std::max(0.25, msg.value("devicePixelRatio", 1.0));
  t.viewportW = msg.value("viewportWidth", 0.0);
  t.viewportH = msg.value("viewportHeight", 0.0);
  t.screenX = msg.value("screenX", 0.0);
  t.screenY = msg.value("screenY", 0.0);
  t.lastSeen = now;
  auto &s = t.scrollers[msg.value("scroller", "document")];
  s.x = msg.value("scrollX", 0.0);
  s.y = msg.value("scrollY", 0.0);
  if (msg.contains("scrollerRect")) {
    const auto &r = msg["scrollerRect"];
    s.rectCss = {r.value("x", 0.0f), r.value("y", 0.0f), r.value("x", 0.0f) + r.value("width", 0.0f),
                 r.value("y", 0.0f) + r.value("height", 0.0f)};
  }
}

std::optional<int> ScrollTracker::ActiveTabFor(HWND hwnd, const RECT &vp) {
  (void)hwnd;
  const double w = vp.right - vp.left, h = vp.bottom - vp.top;
  // Among windows of the same size, the reported screen position decides. It is only
  // approximate (browser chrome is estimated), so it's a tie-breaker, not a filter.
  std::optional<int> best;
  double bestScore = 1e18;
  for (const auto &[windowId, tabId] : activeTabByWindow_) {
    auto it = tabs_.find(tabId);
    if (it == tabs_.end() || it->second.docKey.empty()) continue;
    const auto &t = it->second;
    // The page viewport in physical px must match the browser's content window.
    const double dw = std::abs(t.viewportW * t.dpr - w), dh = std::abs(t.viewportH * t.dpr - h);
    if (dw > 8 || dh > 8) continue;
    const double osScale = t.dpr / std::max(0.25, t.zoom);
    const double dpos = std::abs(t.screenX * osScale - vp.left) + std::abs(t.screenY * osScale - vp.top);
    const double score = (dw + dh) * 100 + dpos;
    if (score < bestScore) {
      bestScore = score;
      best = tabId;
    }
  }
  return best;
}

std::optional<ScrollTracker::Target> ScrollTracker::ResolveBrowser(HWND hwnd, POINT pt) {
  const auto vp = ChromiumViewport(hwnd);
  if (!vp || !PtInRect(&*vp, pt)) return std::nullopt;
  const auto tabId = ActiveTabFor(hwnd, *vp);
  if (!tabId) return std::nullopt;
  const auto &t = tabs_[*tabId];

  // Prefer an inner scroller containing the point, else the document.
  std::string scroller = "document";
  const double cssX = (pt.x - vp->left) / t.dpr, cssY = (pt.y - vp->top) / t.dpr;
  for (const auto &[key, s] : t.scrollers) {
    if (key != "document" && s.rectCss.Contains({static_cast<float>(cssX), static_cast<float>(cssY)})) {
      scroller = key;
      break;
    }
  }

  Target target;
  target.layerKey = "web:" + t.docKey + "#" + scroller;
  target.source = SyncSource::Browser;
  target.hwnd = hwnd;
  Binding b;
  b.source = SyncSource::Browser;
  b.hwnd = hwnd;
  b.docKey = t.docKey;
  b.scroller = scroller;
  if (auto existing = bindings_.find(target.layerKey); existing != bindings_.end()) b.manualDy = existing->second.manualDy;
  bindings_[target.layerKey] = b;
  Layer tmp;
  tmp.isDocument = true;
  UpdateBrowserLayer(tmp, bindings_[target.layerKey]);
  target.viewport = tmp.viewport;
  return target;
}

std::optional<ScrollTracker::Target> ScrollTracker::Resolve(POINT pt) {
  HWND hwnd = TopLevelWindowAt(pt);
  if (!hwnd) return std::nullopt;

  if (IsChromiumBrowser(hwnd)) {
    if (auto t = ResolveBrowser(hwnd, pt)) return t;
    // Extension not installed or page not reachable (e.g. chrome:// or the PDF viewer): fall through.
  }

  // Reuse an existing UIA/optical binding covering this point; binding again would
  // start a second poller on the same content.
  std::string key;
  int slot = -1;
  for (const auto &[k, b] : bindings_) {
    if (b.hwnd != hwnd || b.workerSlot < 0) continue;
    const auto s = worker_->Get(b.workerSlot);
    if (s && s->alive && s->screenRect.Contains({static_cast<float>(pt.x), static_cast<float>(pt.y)})) {
      key = k;
      slot = b.workerSlot;
      break;
    }
  }
  if (slot < 0) slot = worker_->Bind(pt, hwnd, &key);
  if (slot < 0) return std::nullopt;
  const auto snap = worker_->Get(slot);
  if (!snap) return std::nullopt;

  Binding b;
  b.source = snap->source;
  b.hwnd = hwnd;
  b.workerSlot = slot;
  if (auto existing = bindings_.find(key); existing != bindings_.end()) b.manualDy = existing->second.manualDy;
  bindings_[key] = b;

  Target t;
  t.layerKey = key;
  t.source = snap->source;
  t.hwnd = hwnd;
  t.viewport.screenRect = snap->screenRect;
  t.viewport.scrollX = snap->scrollX;
  t.viewport.scrollY = snap->scrollY + b.manualDy;
  t.viewport.scale = 1.0;
  t.viewport.confidence = snap->confidence;
  return t;
}

bool ScrollTracker::UpdateBrowserLayer(Layer &layer, Binding &b) {
  Viewport v = layer.viewport;
  v.visible = false;
  const auto vp = IsWindow(b.hwnd) ? ChromiumViewport(b.hwnd) : std::nullopt;
  if (vp) {
    const auto tabId = ActiveTabFor(b.hwnd, *vp);
    if (tabId) {
      const auto &t = tabs_[*tabId];
      auto s = t.scrollers.find(b.scroller);
      if (t.docKey == b.docKey && s != t.scrollers.end()) {
        v.scale = t.dpr;
        v.scrollX = s->second.x;
        v.scrollY = s->second.y + b.manualDy;
        if (b.scroller == "document") {
          v.screenRect = ToRectF(*vp);
        } else {
          const auto &r = s->second.rectCss;
          v.screenRect = {static_cast<float>(vp->left + r.left * t.dpr), static_cast<float>(vp->top + r.top * t.dpr),
                          static_cast<float>(vp->left + r.right * t.dpr), static_cast<float>(vp->top + r.bottom * t.dpr)};
        }
        v.confidence = 1.0f;
        v.visible = true;
      }
    }
  }
  const bool changed = v.visible != layer.viewport.visible || v.scrollX != layer.viewport.scrollX ||
      v.scrollY != layer.viewport.scrollY || v.scale != layer.viewport.scale ||
      v.screenRect.left != layer.viewport.screenRect.left || v.screenRect.top != layer.viewport.screenRect.top ||
      v.screenRect.right != layer.viewport.screenRect.right || v.screenRect.bottom != layer.viewport.screenRect.bottom;
  layer.viewport = v;
  return changed;
}

bool ScrollTracker::Update(Model &model) {
  bool changed = false;
  SyncSource best = SyncSource::None;
  float bestConf = 0;
  for (const auto &layerPtr : model.Layers()) {
    Layer &layer = *layerPtr;
    if (!layer.isDocument) continue;
    auto it = bindings_.find(layer.key);
    if (it == bindings_.end()) {
      if (layer.viewport.visible) {
        layer.viewport.visible = false;
        changed = true;
      }
      continue;
    }
    Binding &b = it->second;
    if (b.source == SyncSource::Browser) {
      changed |= UpdateBrowserLayer(layer, b);
    } else if (auto snap = worker_->Get(b.workerSlot)) {
      Viewport v = layer.viewport;
      v.screenRect = snap->screenRect;
      v.scrollX = snap->scrollX;
      v.scrollY = snap->scrollY + b.manualDy;
      v.scale = 1.0;
      v.confidence = snap->confidence;
      v.visible = snap->alive && !IsIconic(b.hwnd);
      changed |= v.visible != layer.viewport.visible || v.scrollY != layer.viewport.scrollY ||
          v.scrollX != layer.viewport.scrollX || v.screenRect.left != layer.viewport.screenRect.left ||
          v.screenRect.top != layer.viewport.screenRect.top || v.screenRect.right != layer.viewport.screenRect.right ||
          v.screenRect.bottom != layer.viewport.screenRect.bottom;
      layer.viewport = v;
    }

    // Hide ink when another window covers the document.
    if (layer.viewport.visible) {
      const auto &r = layer.viewport.screenRect;
      const POINT c{static_cast<LONG>((r.left + r.right) / 2), static_cast<LONG>((r.top + r.bottom) / 2)};
      if (TopLevelWindowAt(c) != b.hwnd) {
        layer.viewport.visible = false;
        changed = true;
      }
    }
    if (layer.viewport.visible && !layer.items.empty() && layer.viewport.confidence >= bestConf) {
      best = b.source;
      bestConf = layer.viewport.confidence;
    }
  }
  activeSource_ = best;
  activeConfidence_ = bestConf;
  return changed;
}

void ScrollTracker::NoteActivity(POINT pt) {
  (void)pt;
  worker_->Activity();
}

bool ScrollTracker::Nudge(Model &model, POINT pt, double dyPhysical) {
  for (const auto &layerPtr : model.Layers()) {
    Layer &layer = *layerPtr;
    if (!layer.isDocument || !layer.viewport.visible) continue;
    if (!layer.viewport.screenRect.Contains({static_cast<float>(pt.x), static_cast<float>(pt.y)})) continue;
    auto it = bindings_.find(layer.key);
    if (it == bindings_.end()) continue;
    it->second.manualDy += dyPhysical / std::max(0.01, layer.viewport.scale);
    return true;
  }
  return false;
}

} // namespace sp
