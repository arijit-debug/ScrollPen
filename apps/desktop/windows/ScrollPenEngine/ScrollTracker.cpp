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

constexpr size_t kMaxTabs = 512;
constexpr size_t kMaxScrollersPerTab = 64;

// Schema check for a message from the extension. Field types and sizes must match
// sync-protocol.ts exactly; anything else is dropped before it is read.
bool ValidBrowserMessage(const json &m) {
  if (!m.is_object() || !m.contains("v") || !m["v"].is_number_integer() || m["v"].get<int64_t>() != 1) return false;
  if (!m.contains("type") || !m["type"].is_string()) return false;
  auto has = [&](const char *k) { return m.contains(k); };
  auto integer = [&](const char *k, bool required) {
    if (!has(k)) return !required;
    const auto &v = m[k];
    return v.is_number_integer() && v.get<int64_t>() >= INT32_MIN && v.get<int64_t>() <= INT32_MAX;
  };
  auto number = [&](const json &v) { return v.is_number() && std::isfinite(v.get<double>()); };
  auto num = [&](const char *k) { return !has(k) || number(m[k]); };
  auto str = [&](const char *k, size_t maxLen, bool required) {
    if (!has(k)) return !required;
    return m[k].is_string() && m[k].get_ref<const std::string &>().size() <= maxLen;
  };
  auto boolean = [&](const char *k) { return !has(k) || m[k].is_boolean(); };

  const std::string &type = m["type"].get_ref<const std::string &>();
  if (type == "hello") return str("extVersion", 64, false) && str("userAgent", 1024, false);
  if (type == "focus") return integer("tabId", true) && integer("windowId", true);
  if (type == "zoom") return integer("tabId", true) && has("zoom") && number(m["zoom"]);
  if (type == "navigate") return integer("tabId", true) && has("docKey") && (m["docKey"].is_null() || str("docKey", 8192, true));
  if (type != "scroll") return false;
  if (!integer("tabId", true) || !integer("windowId", false) || !str("docKey", 8192, true) ||
      !str("scroller", 2048, false) || !str("title", 4096, false) || !boolean("visible"))
    return false;
  for (const char *k : {"zoom", "devicePixelRatio", "viewportWidth", "viewportHeight", "screenX", "screenY", "scrollX",
                        "scrollY", "scrollWidth", "scrollHeight", "contentScale", "ts"})
    if (!num(k)) return false;
  if (has("scrollerRect")) {
    const auto &r = m["scrollerRect"];
    if (!r.is_object()) return false;
    for (const char *k : {"x", "y", "width", "height"})
      if (r.contains(k) && !number(r[k])) return false;
  }
  return true;
}

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
// Poll intervals for slots whose layer has ink. UIA reads are cheap; an optical capture is
// a PrintWindow, so it idles slower and speeds up once it sees motion.
constexpr double kUiaInterval = 1.0 / 60;
constexpr double kOpticalActiveInterval = 1.0 / 30;
constexpr double kOpticalIdleInterval = 0.1;
// A browser layer keeps its last matched tab this long while the window is mid-resize and
// the extension hasn't reported the new viewport size yet.
constexpr double kTabMatchGraceSeconds = 1.5;
// Scroller key the extension uses for the picture in an image tab (see reporter.ts).
constexpr char kImageScroller[] = "image";

bool SameViewport(const Viewport &a, const Viewport &b) {
  auto sameRect = [](const RectF &x, const RectF &y) {
    return x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom;
  };
  if (a.visible != b.visible || a.scrollX != b.scrollX || a.scrollY != b.scrollY || a.scale != b.scale ||
      !sameRect(a.screenRect, b.screenRect) || !sameRect(a.clipArea, b.clipArea) || a.clip.size() != b.clip.size())
    return false;
  for (size_t i = 0; i < a.clip.size(); ++i)
    if (!sameRect(a.clip[i], b.clip[i])) return false;
  return true;
}

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
  // Worker thread only.
  bool wanted{false}; // its layer has ink, so keep it current
  double nextPoll{0};
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

  // Slots whose layers currently have ink; only these are polled.
  void SetDemand(std::vector<int> slots) {
    std::sort(slots.begin(), slots.end());
    {
      std::lock_guard lock(mutex_);
      if (slots == demand_) return;
      demand_ = std::move(slots);
    }
    cv_.notify_all();
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

      // Poll slots with ink on their own schedule. Without input hooks (cursor mode) there
      // is no wheel signal, so polling is what keeps ink following the content.
      double now = Now();
      std::vector<WorkerSlot *> due;
      for (size_t i = 0; i < slots_.size(); ++i) {
        auto &s = *slots_[i];
        s.wanted = s.alive && std::binary_search(demand_.begin(), demand_.end(), static_cast<int>(i));
        if (s.wanted && now >= s.nextPoll) due.push_back(&s);
      }
      lock.unlock();
      bool changed = false;
      for (auto *s : due) {
        const bool moved = Poll(*s);
        if (moved) lastActivity_ = Now(); // content is moving: keep sampling fast
        changed |= moved;
        const bool fast = Now() - lastActivity_ < kActiveWindowSeconds;
        s->nextPoll = Now() + (s->source == SyncSource::Uia ? kUiaInterval
                               : fast                         ? kOpticalActiveInterval
                                                              : kOpticalIdleInterval);
      }
      lock.lock();
      if (changed && wake_) {
        auto w = wake_;
        lock.unlock();
        w();
        lock.lock();
      }
      now = Now();
      double wait = 0.5;
      for (auto &s : slots_)
        if (s->wanted) wait = std::min(wait, s->nextPoll - now);
      wait = std::clamp(wait, 0.004, 0.5);
      cv_.wait_for(lock, std::chrono::duration<double>(wait), [&] { return stop_ || !requests_.empty(); });
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

  bool PollOptical(WorkerSlot &s) {
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

  bool Poll(WorkerSlot &s) {
    if (!IsWindow(s.hwnd)) {
      std::lock_guard lock(mutex_);
      s.alive = false;
      return true;
    }
    // A minimized window has nothing to measure; skip it rather than read stale content.
    if (IsIconic(s.hwnd)) return false;
    return s.source == SyncSource::Uia ? PollUia(s) : PollOptical(s);
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  bool stop_{false};
  std::vector<std::shared_ptr<Request>> requests_;
  std::vector<std::unique_ptr<WorkerSlot>> slots_;
  std::vector<int> demand_; // sorted slot indices
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
  if (!ValidBrowserMessage(msg)) return;
  const std::string type = msg.value("type", "");
  const int tabId = msg.value("tabId", -1);
  const double now = Now();
  // Bounded state, however many tabs or scrollers a sender invents.
  if (tabId >= 0 && !tabs_.count(tabId) && tabs_.size() >= kMaxTabs) return;

  if (type == "focus") {
    if (!activeTabByWindow_.count(msg.value("windowId", -1)) && activeTabByWindow_.size() >= kMaxTabs) return;
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
    if (msg.value("visible", true) && (activeTabByWindow_.count(t.windowId) || activeTabByWindow_.size() < kMaxTabs))
      activeTabByWindow_[t.windowId] = tabId;
  }
  if (msg.contains("zoom")) t.zoom = msg.value("zoom", 1.0);
  t.dpr = std::max(0.25, msg.value("devicePixelRatio", 1.0));
  t.viewportW = msg.value("viewportWidth", 0.0);
  t.viewportH = msg.value("viewportHeight", 0.0);
  t.screenX = msg.value("screenX", 0.0);
  t.screenY = msg.value("screenY", 0.0);
  if (msg.contains("title") && msg["title"].is_string()) t.title = Widen(msg["title"].get<std::string>());
  t.lastSeen = now;
  const std::string scroller = msg.value("scroller", "document");
  if (!t.scrollers.count(scroller) && t.scrollers.size() >= kMaxScrollersPerTab) return;
  auto &s = t.scrollers[scroller];
  s.x = msg.value("scrollX", 0.0);
  s.y = msg.value("scrollY", 0.0);
  if (msg.contains("scrollerRect")) {
    const auto &r = msg["scrollerRect"];
    s.rectCss = {r.value("x", 0.0f), r.value("y", 0.0f), r.value("x", 0.0f) + r.value("width", 0.0f),
                 r.value("y", 0.0f) + r.value("height", 0.0f)};
  }
  s.contentScale = std::max(0.001, msg.value("contentScale", 1.0));
}

// Chrome lays out its tab strip and toolbar above the page and any side panel or docked
// DevTools beside it, so the page is the bottom-left part of the window frame, sized like
// the tab's viewport. (The page's own child window, "Chrome Legacy Window", is usually
// hidden or left at a stale position in Chrome, so it can't be used.)
std::optional<RECT> ScrollTracker::PageRect(HWND hwnd, const TabState &t) {
  const RECT f = VisibleFrame(hwnd);
  const LONG w = std::lround(t.viewportW * t.dpr), h = std::lround(t.viewportH * t.dpr);
  if (w <= 0 || h <= 0 || w > f.right - f.left + 8 || h > f.bottom - f.top + 8) return std::nullopt;
  return RECT{f.left, f.bottom - h, f.left + w, f.bottom};
}

std::optional<int> ScrollTracker::ActiveTabFor(HWND hwnd) {
  // Chrome titles each window "<active tab's title> - Google Chrome": a tab whose title
  // doesn't start the window's title isn't the one this window shows.
  const std::wstring windowTitle = CurrentTitle(hwnd);
  const double now = Now();
  std::optional<int> best;
  double bestScore = 1e18;
  for (const auto &[windowId, tabId] : activeTabByWindow_) {
    auto it = tabs_.find(tabId);
    if (it == tabs_.end() || it->second.docKey.empty()) continue;
    const auto &t = it->second;
    if (!t.title.empty() && windowTitle.rfind(t.title, 0) != 0) continue;
    const auto r = PageRect(hwnd, t);
    if (!r) continue;
    // Several windows can show same-sized pages with the same title (the same page twice):
    // then the page's reported screen position decides. The extension computes it as
    // window position + (outer size - inner size), mixing DIPs with zoomed CSS px, so take
    // the zoom back out. It's only a tie-breaker: a zoomed page must never fail to match.
    const double zoom = std::max(0.25, t.zoom), osScale = t.dpr / zoom;
    const double estX = t.screenX + t.viewportW * (1 - zoom) / 2, estY = t.screenY + t.viewportH * (1 - zoom);
    const double dpos = std::abs(estX * osScale - r->left) + std::abs(estY * osScale - r->top);
    // Same place too (e.g. two maximized windows): the one reporting most recently is in use.
    const double score = dpos + std::min(now - t.lastSeen, 60.0) * 2;
    if (score < bestScore) {
      bestScore = score;
      best = tabId;
    }
  }
  return best;
}

bool ScrollTracker::IsChromeAt(POINT pt) {
  const HWND w = TopLevelWindowAt(pt);
  return w && WindowClass(w) == L"Chrome_WidgetWin_1" && ProcessName(w) == L"chrome.exe";
}

std::optional<ScrollTracker::Target> ScrollTracker::ResolveBrowser(HWND hwnd, POINT pt, int tabId, bool probe) {
  const auto &t = tabs_[tabId];
  const auto vp = PageRect(hwnd, t);
  // On the browser's own tab strip or toolbar: nothing to follow, ink stays on screen.
  if (!vp || !PtInRect(&*vp, pt)) return std::nullopt;

  // Prefer an inner scroller containing the point, else the document. The page only
  // reports inner scrollers once they scroll, so ask it about this point (see Refine).
  std::string scroller = "document";
  const double cssX = (pt.x - vp->left) / t.dpr, cssY = (pt.y - vp->top) / t.dpr;
  if (probe && send_)
    send_(json{{"type", "probe"}, {"v", 1}, {"tabId", tabId}, {"x", cssX}, {"y", cssY}}.dump());
  for (const auto &[key, s] : t.scrollers) {
    if (key != "document" && s.rectCss.Contains({static_cast<float>(cssX), static_cast<float>(cssY)})) {
      scroller = key;
      break;
    }
  }
  // An image opened in Chrome: Chrome centres it, so zooming moves it around the page.
  // Ink anywhere on the tab belongs to the image.
  if (t.scrollers.count(kImageScroller)) scroller = kImageScroller;

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

  std::wstring title;
  if (IsChromiumBrowser(hwnd)) {
    // The page in the active tab reports to us: follow it exactly.
    if (const auto tabId = ActiveTabFor(hwnd)) return ResolveBrowser(hwnd, pt, *tabId, true);
    // Pages the extension can't run in (Chrome's PDF viewer, chrome:// pages), or no
    // extension: track by UI Automation or optically below. Chrome shows the active tab's
    // title as the window title; ink drawn now belongs to this tab only.
    title = CurrentTitle(hwnd);
  }

  // Reuse an existing UIA/optical binding covering this point; binding again would
  // start a second poller on the same content.
  std::string key;
  int slot = -1;
  for (const auto &[k, b] : bindings_) {
    if (b.hwnd != hwnd || b.workerSlot < 0 || b.title != title) continue;
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
  b.title = title;
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

std::optional<ScrollTracker::Target> ScrollTracker::Refine(const Target &target, POINT pt) {
  if (target.source != SyncSource::Browser) return std::nullopt;
  auto b = bindings_.find(target.layerKey);
  if (b == bindings_.end() || b->second.scroller != "document" || b->second.tabId < 0) return std::nullopt;
  auto refined = ResolveBrowser(b->second.hwnd, pt, b->second.tabId, false);
  if (!refined || refined->layerKey == target.layerKey) return std::nullopt;
  return refined;
}

std::wstring ScrollTracker::CurrentTitle(HWND hwnd) {
  // InternalGetWindowText reads the cached title without sending the window a message, so
  // a hung browser can't stall us.
  wchar_t buf[512]{};
  InternalGetWindowText(hwnd, buf, 512);
  return buf;
}

bool ScrollTracker::UpdateBrowserLayer(Layer &layer, Binding &b) {
  Viewport v = layer.viewport;
  v.visible = false;
  if (IsWindow(b.hwnd) && !IsIconic(b.hwnd)) {
    auto tabId = ActiveTabFor(b.hwnd);
    const double now = Now();
    if (tabId) {
      b.tabId = *tabId;
      b.lastMatch = now;
    } else if (b.tabId >= 0 && now - b.lastMatch < kTabMatchGraceSeconds) {
      // The window was just resized and the extension's report of the new size is still
      // in flight. Keep the last tab if it's still the visible one in its window.
      auto t = tabs_.find(b.tabId);
      auto active = t != tabs_.end() ? activeTabByWindow_.find(t->second.windowId) : activeTabByWindow_.end();
      if (active != activeTabByWindow_.end() && active->second == b.tabId) tabId = b.tabId;
    }
    if (tabId) {
      const auto &t = tabs_[*tabId];
      const auto vp = PageRect(b.hwnd, t);
      auto s = t.scrollers.find(b.scroller);
      if (vp && t.docKey == b.docKey && s != t.scrollers.end()) {
        v.scale = t.dpr * s->second.contentScale;
        v.scrollX = s->second.x;
        v.scrollY = s->second.y + b.manualDy;
        v.clipArea = {};
        if (b.scroller == "document") {
          v.screenRect = ToRectF(*vp);
        } else {
          // The image only anchors the ink; it shows anywhere on the page.
          if (b.scroller == kImageScroller) v.clipArea = ToRectF(*vp);
          const auto &r = s->second.rectCss;
          v.screenRect = {static_cast<float>(vp->left + r.left * t.dpr), static_cast<float>(vp->top + r.top * t.dpr),
                          static_cast<float>(vp->left + r.right * t.dpr), static_cast<float>(vp->top + r.bottom * t.dpr)};
        }
        v.confidence = 1.0f;
        v.visible = true;
      }
    }
  }
  const bool changed = !SameViewport(v, layer.viewport);
  layer.viewport = v;
  return changed;
}

bool ScrollTracker::Update(Model &model) {
  bool changed = false;
  SyncSource best = SyncSource::None;
  float bestConf = 0;
  std::vector<int> demand;
  for (const auto &layerPtr : model.Layers()) {
    Layer &layer = *layerPtr;
    if (!layer.isDocument) continue;
    const Viewport before = layer.viewport;
    auto it = bindings_.find(layer.key);
    if (it == bindings_.end()) {
      layer.viewport.visible = false;
      changed |= !SameViewport(before, layer.viewport);
      continue;
    }
    Binding &b = it->second;
    if (b.source == SyncSource::Browser) {
      UpdateBrowserLayer(layer, b);
    } else if (auto snap = worker_->Get(b.workerSlot)) {
      if (!layer.items.empty()) demand.push_back(b.workerSlot);
      Viewport &v = layer.viewport;
      v.screenRect = snap->screenRect;
      v.scrollX = snap->scrollX;
      v.scrollY = snap->scrollY + b.manualDy;
      v.scale = 1.0;
      v.confidence = snap->confidence;
      // Ink drawn on one Chrome tab hides while another tab is shown.
      v.visible = snap->alive && !IsIconic(b.hwnd) && (b.title.empty() || CurrentTitle(b.hwnd) == b.title);
    }

    // Clip ink to the parts of the document no other window covers. Partly covered
    // documents keep showing ink in their uncovered parts.
    Viewport &v = layer.viewport;
    v.clip.clear();
    if (v.visible) {
      const auto r = v.ClipArea();
      const RECT area{static_cast<LONG>(r.left), static_cast<LONG>(r.top), static_cast<LONG>(r.right),
                      static_cast<LONG>(r.bottom)};
      for (const auto &u : UncoveredRects(b.hwnd, area)) v.clip.push_back(ToRectF(u));
      if (v.clip.empty()) v.visible = false;
    }
    changed |= !SameViewport(before, v);
    if (v.visible && !layer.items.empty() && v.confidence >= bestConf) {
      best = b.source;
      bestConf = v.confidence;
    }
  }
  worker_->SetDemand(std::move(demand));
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
