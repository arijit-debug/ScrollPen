#include "pch.h"
#include "Engine.h"

#include "InputHooks.h"
#include "Model.h"
#include "Renderer.h"
#include "ScrollTracker.h"
#include "Settings.h"
#include "SyncServer.h"
#include "WindowUtil.h"

#include <shlobj.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace sp {

using json = nlohmann::json;
using Microsoft::WRL::ComPtr;

namespace {

enum : UINT {
  WM_APP_COMMAND = WM_APP + 1, // lParam: std::string* (owned by receiver)
  WM_APP_SYNC,                 // lParam: std::string*
  WM_APP_WHEEL,                // wParam: delta | horizontal<<31, lParam: point
  WM_APP_WHEELSEEN,
  WM_APP_BURSTDOWN, // wParam: button message
  WM_APP_HOLD,      // wParam: 1 down, 0 up
  WM_APP_KEYCMD,    // wParam: KeyCmd, lParam: arg
  WM_APP_CHAR,      // wParam: UTF-16 unit
  WM_APP_WAKE,
  WM_APP_HOOKS_REINSTALLED,
};

enum class KeyCmd : WPARAM {
  Tool,       // lParam: index into kTools
  Color,      // lParam: palette index
  Undo,
  Redo,
  Clear,
  Exit,
  Whiteboard,
  Blackboard,
  SizeDown,
  SizeUp,
  Nudge,      // lParam: signed px
  TextBackspace,
  TextEnter,
  TextCommit,
  TextCancel,
};

enum TimerId : UINT_PTR { kTimerBurst = 1, kTimerAnim, kTimerCaret, kTimerTrack };

const char *const kTools[] = {"pen", "highlighter", "laser", "eraser", "line", "arrow", "rect", "ellipse", "text"};

constexpr wchar_t kOverlayClass[] = L"ScrollPenOverlay";
constexpr wchar_t kControllerClass[] = L"ScrollPenController";
constexpr double kBurstSeconds = 0.35; // keep the overlay click-through this long after the last wheel event

double Now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

POINT UnpackPoint(LPARAM lp) {
  return {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
}

LPARAM PackPoint(POINT p) {
  return MAKELPARAM(static_cast<short>(p.x), static_cast<short>(p.y));
}

bool IsPromotedFromPenOrTouch() {
  // Mouse messages synthesized from pen/touch carry this signature; we handle those via WM_POINTER.
  return (GetMessageExtraInfo() & 0xFFFFFF00) == 0xFF515700;
}

std::wstring AppDataDir() {
  PWSTR base = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
    dir = std::wstring(base) + L"\\ScrollPen";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
  }
  return dir;
}

bool WriteFileAtomic(const std::wstring &path, const std::string &data) {
  const std::wstring tmp = path + L".tmp";
  HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
  CloseHandle(f);
  return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

std::optional<std::string> ReadSmallFile(const std::wstring &path, DWORD maxBytes = 256 * 1024) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return std::nullopt;
  LARGE_INTEGER size{};
  std::optional<std::string> out;
  if (GetFileSizeEx(f, &size) && size.QuadPart <= maxBytes) {
    std::string s(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (ReadFile(f, s.data(), static_cast<DWORD>(s.size()), &read, nullptr) && read == s.size()) out = std::move(s);
  }
  CloseHandle(f);
  return out;
}

void SendWheel(int delta, bool horizontal) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
  in.mi.mouseData = static_cast<DWORD>(delta);
  in.mi.dwExtraInfo = InputHooks::kInjectedTag;
  SendInput(1, &in, sizeof(in));
}

void SendButton(UINT buttonMsg) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = buttonMsg == WM_RBUTTONDOWN ? MOUSEEVENTF_RIGHTDOWN
      : buttonMsg == WM_MBUTTONDOWN           ? MOUSEEVENTF_MIDDLEDOWN
                                              : MOUSEEVENTF_LEFTDOWN;
  in.mi.dwExtraInfo = InputHooks::kInjectedTag;
  SendInput(1, &in, sizeof(in));
}

} // namespace

class Engine::Impl {
 public:
  Impl(HWND dock, EventSink sink) : dock_(dock), sink_(std::move(sink)) {}

  void Start() {
    thread_ = std::thread([this] { ThreadMain(); });
    std::unique_lock lock(startMutex_);
    startCv_.wait(lock, [&] { return started_; });
  }

  void Stop() {
    if (controller_) PostMessageW(controller_, WM_CLOSE, 0, 0);
    if (thread_.joinable()) thread_.join();
  }

  void Post(const std::string &cmd) {
    if (!controller_ || !PostMessageW(controller_, WM_APP_COMMAND, 0, reinterpret_cast<LPARAM>(new std::string(cmd)))) {
      // Engine not running; drop the command.
    }
  }

 private:
  struct Overlay {
    HWND hwnd{};
    RECT rect{};
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> target;
    ComPtr<ID2D1Bitmap1> cache;
    ComPtr<IDCompositionTarget> compTarget;
    ComPtr<IDCompositionVisual> visual;
    bool cacheValid{false};
  };

  enum class Tool { Pen, Highlighter, Laser, Eraser, Line, Arrow, Rect, Ellipse, Text };

  // ---- thread & window plumbing ----------------------------------------------------

  void ThreadMain() {
    // Windows and coordinates on this thread must be physical pixels on every monitor,
    // whatever awareness the host process ended up with.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &Impl::StaticProc;
    wc.hInstance = inst;
    wc.lpszClassName = kControllerClass;
    RegisterClassExW(&wc);
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassExW(&wc);

    // A real (hidden) top-level window, not message-only, so it receives WM_DISPLAYCHANGE.
    controller_ = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"ScrollPen", WS_POPUP, 0, 0, 0, 0, nullptr,
                                  nullptr, inst, this);
    LoadSettings();
    live_.laserFadeSeconds = settings_.laserFadeSeconds;
    device_.Create();
    renderer_ = std::make_unique<Renderer>(device_);
    tracker_.SetWakeCallback([hwnd = controller_] { PostMessageW(hwnd, WM_APP_WAKE, 0, 0); });
    CreateOverlays();
    RegisterHotkeys();
    StartSyncServer();
    ApplyDockSettings();
    SetTimer(controller_, kTimerTrack, 250, nullptr);
    if (settings_.startActive) SetActive(true);

    {
      std::lock_guard lock(startMutex_);
      started_ = true;
    }
    startCv_.notify_all();
    EmitState();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      DispatchMessageW(&msg);
      // Coalesce: render once after the queue is drained.
      if (!PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE) && needsRender_) RenderAll();
    }

    hooks_.Stop();
    sync_.Stop();
    for (auto &o : overlays_) DestroyWindow(o.hwnd);
    overlays_.clear();
    CoUninitialize();
  }

  static LRESULT CALLBACK StaticProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Impl *self = nullptr;
    if (msg == WM_NCCREATE) {
      self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<Impl *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Proc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  LRESULT Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (hwnd == controller_) return ControllerProc(msg, wp, lp);
    return OverlayProc(hwnd, msg, wp, lp);
  }

  LRESULT ControllerProc(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_APP_COMMAND: {
        std::unique_ptr<std::string> s(reinterpret_cast<std::string *>(lp));
        HandleCommand(*s);
        return 0;
      }
      case WM_APP_SYNC: {
        std::unique_ptr<std::string> s(reinterpret_cast<std::string *>(lp));
        auto j = json::parse(*s, nullptr, false);
        if (!j.is_discarded()) {
          tracker_.OnBrowserMessage(j);
          if (tracker_.Update(model_)) InvalidateCaches();
        }
        return 0;
      }
      case WM_APP_WHEEL: {
        const bool horizontal = (wp >> 31) & 1;
        const int delta = static_cast<short>(wp & 0xFFFF);
        BeginBurst();
        SendWheel(delta, horizontal);
        tracker_.NoteActivity(UnpackPoint(lp));
        return 0;
      }
      case WM_APP_WHEELSEEN:
        if (burst_) SetTimer(controller_, kTimerBurst, static_cast<UINT>(kBurstSeconds * 1000), nullptr);
        tracker_.NoteActivity({});
        return 0;
      case WM_APP_BURSTDOWN:
        EndBurst();
        SendButton(static_cast<UINT>(wp));
        return 0;
      case WM_APP_HOLD:
        holdDown_ = wp != 0;
        UpdateClickThrough();
        EmitState();
        return 0;
      case WM_APP_KEYCMD:
        HandleKeyCmd(static_cast<KeyCmd>(wp), lp);
        return 0;
      case WM_APP_CHAR:
        if (live_.text) {
          live_.text->text.push_back(static_cast<wchar_t>(wp));
          live_.caretOn = true;
          Invalidate();
        }
        return 0;
      case WM_APP_WAKE:
        if (tracker_.Update(model_)) InvalidateCaches();
        return 0;
      case WM_APP_HOOKS_REINSTALLED:
        Emit({{"ev", "log"}, {"level", "warn"}, {"message", "Input hooks were reinstalled after Windows removed them."}});
        return 0;
      case WM_TIMER:
        OnTimer(wp);
        return 0;
      case WM_HOTKEY:
        OnHotkey(static_cast<int>(wp));
        return 0;
      case WM_DISPLAYCHANGE:
      case WM_DPICHANGED:
      case WM_SETTINGCHANGE:
        if (msg != WM_SETTINGCHANGE || wp == SPI_SETWORKAREA) {
          CreateOverlays();
          Invalidate(true);
        }
        return 0;
      case WM_CLOSE:
        for (auto &[id, _] : hotkeyIds_) UnregisterHotKey(controller_, id);
        DestroyWindow(controller_);
        return 0;
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(controller_, msg, wp, lp);
  }

  // ---- overlays ----------------------------------------------------------------------

  void CreateOverlays() {
    for (auto &o : overlays_) DestroyWindow(o.hwnd);
    overlays_.clear();
    renderer_->ClearCache();
    if (device_.IsLost() && !device_.Create()) {
      Emit({{"ev", "error"}, {"message", "No usable graphics device."}});
      return;
    }

    for (const auto &m : EnumerateMonitors()) {
      Overlay o;
      o.rect = m.rect;
      const int w = m.rect.right - m.rect.left, h = m.rect.bottom - m.rect.top;
      o.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                   WS_EX_LAYERED | WS_EX_TRANSPARENT,
                               kOverlayClass, L"ScrollPen Ink", WS_POPUP, m.rect.left, m.rect.top, w, h, nullptr,
                               nullptr, GetModuleHandleW(nullptr), this);
      if (!o.hwnd) continue;
      SetLayeredWindowAttributes(o.hwnd, 0, 255, LWA_ALPHA);

      DXGI_SWAP_CHAIN_DESC1 desc{};
      desc.Width = static_cast<UINT>(w);
      desc.Height = static_cast<UINT>(h);
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.Scaling = DXGI_SCALING_STRETCH;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      if (FAILED(device_.DxgiFactory()->CreateSwapChainForComposition(device_.D3D(), &desc, nullptr, &o.swap)) ||
          FAILED(device_.D2D()->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &o.dc))) {
        DestroyWindow(o.hwnd);
        continue;
      }
      o.dc->SetDpi(96, 96);
      ComPtr<IDXGISurface> surface;
      o.swap->GetBuffer(0, IID_PPV_ARGS(&surface));
      const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      o.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &o.target);
      const auto cacheProps =
          D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
      o.dc->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), nullptr, 0, &cacheProps, &o.cache);

      device_.DComp()->CreateTargetForHwnd(o.hwnd, TRUE, &o.compTarget);
      device_.DComp()->CreateVisual(&o.visual);
      o.visual->SetContent(o.swap.Get());
      o.compTarget->SetRoot(o.visual.Get());
      ShowWindow(o.hwnd, SW_SHOWNOACTIVATE);
      overlays_.push_back(std::move(o));
    }
    device_.DComp()->Commit();
    UpdateClickThrough();
    RaiseDock();
    Invalidate(true);
  }

  bool IsOverlay(HWND hwnd) const {
    for (const auto &o : overlays_)
      if (o.hwnd == hwnd) return true;
    return false;
  }

  void RaiseDock() {
    if (!dock_) return;
    SetWindowPos(dock_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }

  void UpdateClickThrough() {
    const bool through = !active_ || holdDown_ || burst_;
    if (through == clickThrough_ && !overlaysStyleDirty_) return;
    clickThrough_ = through;
    overlaysStyleDirty_ = false;
    for (auto &o : overlays_) {
      LONG_PTR ex = GetWindowLongPtrW(o.hwnd, GWL_EXSTYLE);
      ex = through ? (ex | WS_EX_TRANSPARENT) : (ex & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
      SetWindowLongPtrW(o.hwnd, GWL_EXSTYLE, ex);
      SetWindowPos(o.hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
  }

  void Invalidate(bool caches = false) {
    if (caches) InvalidateCaches();
    needsRender_ = true;
  }

  void InvalidateCaches() {
    for (auto &o : overlays_) o.cacheValid = false;
    needsRender_ = true;
  }

  void RenderAll() {
    needsRender_ = false;
    if (device_.IsLost()) {
      device_.Create();
      CreateOverlays();
    }
    const double now = Now();
    const bool showInk = inkVisible_;
    for (auto &o : overlays_) {
      if (!o.dc || !o.target) continue;
      const auto toLocal = D2D1::Matrix3x2F::Translation(static_cast<float>(-o.rect.left), static_cast<float>(-o.rect.top));
      if (!o.cacheValid) {
        o.dc->SetTarget(o.cache.Get());
        o.dc->BeginDraw();
        o.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        o.dc->SetTransform(toLocal);
        if (showInk) renderer_->DrawCommitted(o.dc.Get(), model_, live_, board_);
        o.dc->EndDraw();
        o.cacheValid = true;
      }
      o.dc->SetTarget(o.target.Get());
      o.dc->BeginDraw();
      o.dc->SetTransform(D2D1::Matrix3x2F::Identity());
      Renderer::DrawBackground(o.dc.Get(), board_);
      o.dc->DrawBitmap(o.cache.Get());
      o.dc->SetTransform(toLocal);
      if (showInk) renderer_->DrawLive(o.dc.Get(), live_, board_, now);
      const HRESULT hr = o.dc->EndDraw();
      if (hr == D2DERR_RECREATE_TARGET) {
        device_.Reset();
        needsRender_ = true;
        return;
      }
      o.swap->Present(0, 0);
    }
  }

  // ---- pointer input -----------------------------------------------------------------

  LRESULT OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
      case WM_NCHITTEST:
        return HTCLIENT;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_MOUSEMOVE: {
        if (IsPromotedFromPenOrTouch()) return 0;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hwnd, &p);
        if (msg == WM_LBUTTONDOWN) {
          SetCapture(hwnd);
          Begin(p, 1.0f, false);
        } else if (msg == WM_MOUSEMOVE) {
          if (pointerDown_) Move(p, 1.0f);
          else Hover(p);
        } else {
          ReleaseCapture();
          End();
        }
        return 0;
      }
      case WM_CAPTURECHANGED:
        if (pointerDown_ && pointerId_ == 0) End();
        return 0;
      case WM_MOUSEWHEEL:
      case WM_MOUSEHWHEEL: {
        // Reached the overlay without passing the hook (e.g. some precision-touchpad paths).
        BeginBurst();
        SendWheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
        return 0;
      }
      case WM_POINTERDOWN:
      case WM_POINTERUPDATE:
      case WM_POINTERUP:
      case WM_POINTERCAPTURECHANGED:
        if (OnPointer(msg, wp)) return 0;
        break;
      case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, tool_ == Tool::Text ? IDC_IBEAM : IDC_CROSS));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  bool OnPointer(UINT msg, WPARAM wp) {
    const UINT32 id = GET_POINTERID_WPARAM(wp);
    POINTER_INPUT_TYPE type = PT_POINTER;
    if (!GetPointerType(id, &type) || (type != PT_PEN && type != PT_TOUCH)) return false;

    if (msg == WM_POINTERCAPTURECHANGED) {
      if (pointerDown_ && pointerId_ == id) End();
      return true;
    }
    if (type == PT_TOUCH && !IS_POINTER_PRIMARY_WPARAM(wp)) return true; // one finger draws

    auto sample = [&](const POINTER_INFO &pi, const POINTER_PEN_INFO *pen) {
      float pressure = 1.0f;
      if (pen && (pen->penMask & PEN_MASK_PRESSURE)) pressure = std::clamp(pen->pressure / 1024.0f, 0.05f, 1.0f);
      return std::pair<POINT, float>{pi.ptPixelLocation, pressure};
    };

    if (type == PT_PEN) {
      POINTER_PEN_INFO pen{};
      if (!GetPointerPenInfo(id, &pen)) return true;
      const bool eraserEnd = (pen.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
      if (msg == WM_POINTERDOWN) {
        pointerId_ = id;
        const auto [p, pr] = sample(pen.pointerInfo, &pen);
        Begin(p, pr, eraserEnd);
      } else if (msg == WM_POINTERUPDATE) {
        if (pointerDown_ && pointerId_ == id) {
          // Coalesced samples since the last message, oldest last in the array.
          UINT32 count = 0;
          GetPointerPenInfoHistory(id, &count, nullptr);
          std::vector<POINTER_PEN_INFO> hist(std::max<UINT32>(count, 1));
          if (count > 0 && GetPointerPenInfoHistory(id, &count, hist.data())) {
            for (UINT32 i = count; i-- > 0;) {
              const auto [p, pr] = sample(hist[i].pointerInfo, &hist[i]);
              Move(p, pr);
            }
          } else {
            const auto [p, pr] = sample(pen.pointerInfo, &pen);
            Move(p, pr);
          }
        } else {
          Hover(pen.pointerInfo.ptPixelLocation);
        }
      } else if (msg == WM_POINTERUP && pointerId_ == id) {
        End();
      }
      return true;
    }

    POINTER_INFO pi{};
    if (!GetPointerInfo(id, &pi)) return true;
    if (msg == WM_POINTERDOWN) {
      pointerId_ = id;
      Begin(pi.ptPixelLocation, 1.0f, false);
    } else if (msg == WM_POINTERUPDATE && pointerDown_ && pointerId_ == id) {
      Move(pi.ptPixelLocation, 1.0f);
    } else if (msg == WM_POINTERUP && pointerId_ == id) {
      End();
    }
    return true;
  }

  static ItemKind KindFor(Tool t) {
    switch (t) {
      case Tool::Highlighter:
        return ItemKind::Highlight;
      case Tool::Line:
        return ItemKind::Line;
      case Tool::Arrow:
        return ItemKind::Arrow;
      case Tool::Rect:
        return ItemKind::Rect;
      case Tool::Ellipse:
        return ItemKind::Ellipse;
      case Tool::Text:
        return ItemKind::Text;
      default:
        return ItemKind::Stroke;
    }
  }

  static bool IsShape(Tool t) {
    return t == Tool::Line || t == Tool::Arrow || t == Tool::Rect || t == Tool::Ellipse;
  }

  float WidthFor(Tool t) const {
    if (t == Tool::Highlighter) return settings_.highlighterWidth;
    if (IsShape(t)) return settings_.shapeWidth;
    return settings_.penWidth;
  }

  void Hover(POINT p) {
    if (tool_ == Tool::Eraser) {
      live_.eraserCursor = PointF{static_cast<float>(p.x), static_cast<float>(p.y)};
      Invalidate();
    } else if (live_.eraserCursor) {
      live_.eraserCursor.reset();
      Invalidate();
    }
  }

  void Begin(POINT p, float pressure, bool eraserEnd) {
    pointerDown_ = true;
    strokeTool_ = eraserEnd ? Tool::Eraser : tool_;
    const PointF pt{static_cast<float>(p.x), static_cast<float>(p.y)};
    if (live_.text && strokeTool_ != Tool::Text) CommitText();

    target_.reset();
    if (settings_.documentMode && board_ == Board::None && strokeTool_ != Tool::Laser && strokeTool_ != Tool::Eraser) {
      target_ = tracker_.Resolve(p);
      const char *src = target_ ? SyncSourceName(target_->source) : "none";
      if (lastSyncSource_ != src) {
        lastSyncSource_ = src;
        EmitState();
      }
    }

    Item item;
    item.kind = KindFor(strokeTool_);
    item.style.color = settings_.color;
    item.style.width = WidthFor(strokeTool_);
    item.style.fill = settings_.fill;
    item.style.fontSize = settings_.fontSize;

    switch (strokeTool_) {
      case Tool::Pen:
      case Tool::Highlighter:
        item.points.push_back({pt.x, pt.y, pressure});
        live_.active = item;
        break;
      case Tool::Laser:
        live_.laser.push_back({{pt, Now()}});
        StartAnim();
        break;
      case Tool::Eraser:
        live_.eraserCursor = pt;
        EraseAt(pt);
        break;
      case Tool::Text:
        if (live_.text) CommitText();
        item.points.push_back({pt.x, pt.y - settings_.fontSize * 0.65f, 1});
        live_.text = item;
        textEditing_ = true;
        live_.caretOn = true;
        SetTimer(controller_, kTimerCaret, 530, nullptr);
        EmitState();
        break;
      default: // shapes
        item.points.push_back({pt.x, pt.y, 1});
        item.points.push_back({pt.x, pt.y, 1});
        live_.active = item;
        break;
    }
    Invalidate();
  }

  void Move(POINT p, float pressure) {
    const PointF pt{static_cast<float>(p.x), static_cast<float>(p.y)};
    switch (strokeTool_) {
      case Tool::Pen:
      case Tool::Highlighter:
        if (live_.active) {
          auto &pts = live_.active->points;
          const auto &last = pts.back();
          if (std::abs(last.x - pt.x) + std::abs(last.y - pt.y) >= 0.75f) pts.push_back({pt.x, pt.y, pressure});
          else pts.back().pressure = std::max(pts.back().pressure, pressure);
        }
        break;
      case Tool::Laser:
        if (!live_.laser.empty()) live_.laser.back().push_back({pt, Now()});
        StartAnim();
        break;
      case Tool::Eraser:
        live_.eraserCursor = pt;
        EraseAt(pt);
        break;
      case Tool::Text:
        break;
      default:
        if (live_.active && live_.active->points.size() == 2) {
          live_.active->points[1] = Snap(live_.active->points[0], pt, strokeTool_);
        }
        break;
    }
    Invalidate();
  }

  // Shift: lines snap to 45 degrees, rectangles become squares, ellipses circles.
  static InkPoint Snap(const InkPoint &a, PointF b, Tool tool) {
    if (GetKeyState(VK_SHIFT) >= 0) return {b.x, b.y, 1};
    const float dx = b.x - a.x, dy = b.y - a.y;
    if (tool == Tool::Line || tool == Tool::Arrow) {
      const float len = std::sqrt(dx * dx + dy * dy);
      const float angle = std::round(std::atan2(dy, dx) / (3.14159265f / 4)) * (3.14159265f / 4);
      return {a.x + len * std::cos(angle), a.y + len * std::sin(angle), 1};
    }
    const float side = std::max(std::abs(dx), std::abs(dy));
    return {a.x + (dx < 0 ? -side : side), a.y + (dy < 0 ? -side : side), 1};
  }

  void End() {
    if (!pointerDown_) return;
    pointerDown_ = false;
    pointerId_ = 0;
    switch (strokeTool_) {
      case Tool::Pen:
      case Tool::Highlighter:
        if (live_.active) CommitItem(std::move(*live_.active));
        live_.active.reset();
        break;
      case Tool::Eraser:
        CommitErase();
        if (tool_ != Tool::Eraser) live_.eraserCursor.reset();
        break;
      case Tool::Laser:
      case Tool::Text:
        break;
      default:
        if (live_.active) {
          const auto &a = live_.active->points[0], &b = live_.active->points[1];
          if (std::abs(a.x - b.x) + std::abs(a.y - b.y) >= 3) CommitItem(std::move(*live_.active));
        }
        live_.active.reset();
        break;
    }
    Invalidate();
  }

  void CommitItem(Item item) {
    std::string layerKey = "screen";
    if (board_ != Board::None) {
      layerKey = "board";
      model_.GetOrCreateLayer(layerKey, false);
    } else if (target_) {
      Layer &layer = model_.GetOrCreateDocumentLayer(target_->layerKey);
      if (layer.items.empty() || !layer.viewport.visible) layer.viewport = target_->viewport;
      const float scale = static_cast<float>(std::max(0.01, layer.viewport.scale));
      for (auto &p : item.points) {
        const PointF l = layer.ToLayer({p.x, p.y});
        p.x = l.x;
        p.y = l.y;
      }
      item.style.width /= scale;
      item.style.fontSize /= scale;
      layerKey = layer.key;
    }
    model_.AddItem(layerKey, std::move(item));
    InvalidateCaches();
    EmitState();
  }

  void CommitText() {
    if (!live_.text) return;
    Item t = std::move(*live_.text);
    live_.text.reset();
    textEditing_ = false;
    KillTimer(controller_, kTimerCaret);
    if (!t.text.empty()) CommitItem(std::move(t));
    Invalidate();
    EmitState();
  }

  void EraseAt(PointF pt) {
    bool hit = false;
    for (const auto &layer : model_.Layers()) {
      const bool isBoard = layer->key == "board";
      if ((board_ != Board::None) != isBoard) continue;
      if (layer->isDocument && !layer->viewport.visible) continue;
      for (const auto &item : layer->items) {
        if (std::find(live_.hidden.begin(), live_.hidden.end(), item.id) != live_.hidden.end()) continue;
        if (ItemHit(item, *layer, pt, live_.eraserRadius)) {
          live_.hidden.push_back(item.id);
          erasing_[layer->key].push_back(item.id);
          hit = true;
        }
      }
    }
    if (hit) InvalidateCaches();
  }

  void CommitErase() {
    for (const auto &[key, ids] : erasing_) {
      model_.RemoveItems(key, ids);
      for (auto id : ids) renderer_->Forget(id);
    }
    erasing_.clear();
    live_.hidden.clear();
    InvalidateCaches();
    EmitState();
  }

  // ---- modes -------------------------------------------------------------------------

  void SetActive(bool active) {
    if (active == active_) return;
    if (!active) {
      if (pointerDown_) End();
      CommitText();
      live_.eraserCursor.reset();
    }
    active_ = active;
    hooksDrawing_ = active;
    holdDown_ = false;
    EndBurst();
    // Global hooks exist only while drawing: in cursor mode ScrollPen sees no input at all.
    if (active) StartHooks();
    else hooks_.Stop();
    UpdateClickThrough();
    RaiseDock();
    Invalidate();
    EmitState();
  }

  void BeginBurst() {
    burst_ = true;
    UpdateClickThrough();
    SetTimer(controller_, kTimerBurst, static_cast<UINT>(kBurstSeconds * 1000), nullptr);
  }

  void EndBurst() {
    KillTimer(controller_, kTimerBurst);
    if (!burst_) return;
    burst_ = false;
    UpdateClickThrough();
  }

  void StartAnim() {
    if (!animating_) {
      animating_ = true;
      SetTimer(controller_, kTimerAnim, 16, nullptr);
    }
  }

  void OnTimer(WPARAM id) {
    switch (id) {
      case kTimerBurst:
        EndBurst();
        break;
      case kTimerAnim: {
        const double now = Now();
        // Drop fully faded trails.
        live_.laser.erase(std::remove_if(live_.laser.begin(), live_.laser.end(),
                                         [&](const std::vector<LaserPoint> &t) {
                                           return t.empty() || now - t.back().t > live_.laserFadeSeconds;
                                         }),
                          live_.laser.end());
        if (live_.laser.empty() && !(pointerDown_ && strokeTool_ == Tool::Laser)) {
          animating_ = false;
          KillTimer(controller_, kTimerAnim);
        }
        Invalidate();
        break;
      }
      case kTimerCaret:
        live_.caretOn = !live_.caretOn;
        Invalidate();
        break;
      case kTimerTrack: {
        // Document layers follow their windows even when not drawing.
        if (tracker_.Update(model_)) InvalidateCaches();
        const bool anyDoc = std::any_of(model_.Layers().begin(), model_.Layers().end(),
                                        [](const auto &l) { return l->isDocument && !l->items.empty(); });
        SetTimer(controller_, kTimerTrack, anyDoc ? 16 : 250, nullptr);
        const char *src = SyncSourceName(tracker_.ActiveSource());
        if (src != lastSyncSource_ && anyDoc) {
          lastSyncSource_ = src;
          EmitState();
        }
        break;
      }
    }
  }

  // ---- hooks -------------------------------------------------------------------------

  void StartHooks() {
    holdVk_ = ParseKeyName(settings_.clickThroughKey);
    const HWND controller = controller_;
    InputHooks::Callbacks cb;
    cb.onMouse = [this, controller](WPARAM msg, const MSLLHOOKSTRUCT &m) -> bool {
      if (!hooksDrawing_ || holdDown_) return false;
      switch (msg) {
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
          if (burst_) {
            PostMessageW(controller, WM_APP_WHEELSEEN, 0, 0);
            return false; // overlay is already click-through: let it scroll natively
          }
          if (!IsOverlayAt(m.pt)) return false; // e.g. over the dock
          const WPARAM wp = (static_cast<WPARAM>(static_cast<WORD>(HIWORD(m.mouseData)))) |
              (msg == WM_MOUSEHWHEEL ? (WPARAM{1} << 31) : 0);
          PostMessageW(controller, WM_APP_WHEEL, wp, PackPoint(m.pt));
          return true;
        }
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
          if (burst_ && !IsDockAt(m.pt)) {
            // The user started drawing mid-scroll: restore the overlay first, then replay the click onto it.
            PostMessageW(controller, WM_APP_BURSTDOWN, msg, 0);
            return true;
          }
          return false;
      }
      return false;
    };
    cb.onKey = [this, controller](WPARAM msg, const KBDLLHOOKSTRUCT &k) -> bool {
      return OnHookKey(controller, msg, k);
    };
    cb.onReinstalled = [controller] { PostMessageW(controller, WM_APP_HOOKS_REINSTALLED, 0, 0); };
    if (!hooks_.Start(std::move(cb)))
      Emit({{"ev", "error"}, {"message", "Could not install input hooks; scrolling while drawing may not work."}});
  }

  // Hook thread. Must be fast: only atomics, PostMessage and pure lookups.
  bool IsOverlayAt(POINT pt) const {
    const HWND h = WindowFromPoint(pt);
    wchar_t cls[32]{};
    return h && GetClassNameW(h, cls, 32) && wcscmp(cls, kOverlayClass) == 0 && IsOwnWindow(h);
  }

  bool IsDockAt(POINT pt) const {
    const HWND h = WindowFromPoint(pt);
    return h && dock_ && (h == dock_ || GetAncestor(h, GA_ROOT) == dock_);
  }

  bool OnHookKey(HWND controller, WPARAM msg, const KBDLLHOOKSTRUCT &k) {
    if (!hooksDrawing_) return false;
    const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    const unsigned vk = k.vkCode;
    const auto post = [&](KeyCmd cmd, LPARAM arg = 0) {
      PostMessageW(controller, WM_APP_KEYCMD, static_cast<WPARAM>(cmd), arg);
    };

    // Swallow the key-up of every key-down we swallowed so apps never see half a keystroke.
    if (!down) {
      if (vk < 256 && swallowed_[vk]) {
        swallowed_[vk] = false;
        if (vk == holdVk_ && holdDown_) PostMessageW(controller, WM_APP_HOLD, 0, 0);
        return true;
      }
      return false;
    }

    const auto swallow = [&] {
      if (vk < 256) swallowed_[vk] = true;
      return true;
    };
    const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
    const bool shift = GetAsyncKeyState(VK_SHIFT) < 0;
    const bool alt = GetAsyncKeyState(VK_MENU) < 0;
    const bool win = GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
    const bool modifierKey = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
        vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
        vk == VK_LWIN || vk == VK_RWIN;
    if (modifierKey || win) return false;

    if (textEditing_) {
      if (vk == VK_ESCAPE) return post(KeyCmd::TextCommit), swallow();
      if (vk == VK_RETURN) return post(ctrl ? KeyCmd::TextCommit : KeyCmd::TextEnter), swallow();
      if (vk == VK_BACK) return post(KeyCmd::TextBackspace), swallow();
      if (ctrl && !alt) return false; // leave Ctrl shortcuts alone
      BYTE state[256]{};
      if (shift) state[VK_SHIFT] = 0x80;
      if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 0x01;
      if (ctrl && alt) state[VK_CONTROL] = state[VK_MENU] = 0x80; // AltGr
      const HWND fg = GetForegroundWindow();
      const HKL layout = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);
      wchar_t buf[8]{};
      // Flag 0x4: don't change the keyboard state (keeps dead keys working in the target app).
      const int n = ToUnicodeEx(vk, k.scanCode, state, buf, 8, 0x4, layout);
      for (int i = 0; i < n; ++i)
        if (buf[i] >= 0x20) PostMessageW(controller, WM_APP_CHAR, buf[i], 0);
      return n != 0 ? swallow() : false;
    }

    if (vk == holdVk_ && holdVk_ != 0) {
      if (!holdDown_) PostMessageW(controller, WM_APP_HOLD, 1, 0);
      return swallow();
    }
    if (alt) return false;

    if (ctrl) {
      if (vk == 'Z' && !shift) return post(KeyCmd::Undo), swallow();
      if (vk == 'Y' || (vk == 'Z' && shift)) return post(KeyCmd::Redo), swallow();
      if (vk == VK_UP) return post(KeyCmd::Nudge, shift ? -1 : -10), swallow();
      if (vk == VK_DOWN) return post(KeyCmd::Nudge, shift ? 1 : 10), swallow();
      return false;
    }
    if (shift && vk != VK_OEM_4 && vk != VK_OEM_6) return false;

    switch (vk) {
      case 'P': return post(KeyCmd::Tool, 0), swallow();
      case 'H': return post(KeyCmd::Tool, 1), swallow();
      case 'L': return post(KeyCmd::Tool, 2), swallow();
      case 'E': return post(KeyCmd::Tool, 3), swallow();
      case 'I': return post(KeyCmd::Tool, 4), swallow();
      case 'A': return post(KeyCmd::Tool, 5), swallow();
      case 'R': return post(KeyCmd::Tool, 6), swallow();
      case 'O': return post(KeyCmd::Tool, 7), swallow();
      case 'T': return post(KeyCmd::Tool, 8), swallow();
      case 'W': return post(KeyCmd::Whiteboard), swallow();
      case 'K': return post(KeyCmd::Blackboard), swallow();
      case VK_ESCAPE: return post(KeyCmd::Exit), swallow();
      case VK_DELETE: return post(KeyCmd::Clear), swallow();
      case VK_OEM_4: return post(KeyCmd::SizeDown), swallow(); // [
      case VK_OEM_6: return post(KeyCmd::SizeUp), swallow();   // ]
      default:
        if (vk >= '1' && vk <= '9') return post(KeyCmd::Color, static_cast<LPARAM>(vk - '1')), swallow();
        return false;
    }
  }

  void HandleKeyCmd(KeyCmd cmd, LPARAM arg) {
    switch (cmd) {
      case KeyCmd::Tool:
        if (arg >= 0 && arg < static_cast<LPARAM>(std::size(kTools))) SetTool(kTools[arg]);
        break;
      case KeyCmd::Color:
        if (arg >= 0 && static_cast<size_t>(arg) < settings_.palette.size()) {
          settings_.color = settings_.palette[static_cast<size_t>(arg)];
          SaveSettings();
          EmitState();
        }
        break;
      case KeyCmd::Undo:
        Undo();
        break;
      case KeyCmd::Redo:
        Redo();
        break;
      case KeyCmd::Clear:
        ClearAll();
        break;
      case KeyCmd::Exit:
        SetActive(false);
        break;
      case KeyCmd::Whiteboard:
        SetBoard(board_ == Board::White ? Board::None : Board::White);
        break;
      case KeyCmd::Blackboard:
        SetBoard(board_ == Board::Black ? Board::None : Board::Black);
        break;
      case KeyCmd::SizeDown:
      case KeyCmd::SizeUp: {
        float &w = tool_ == Tool::Highlighter ? settings_.highlighterWidth
            : IsShape(tool_)                  ? settings_.shapeWidth
            : tool_ == Tool::Text             ? settings_.fontSize
                                              : settings_.penWidth;
        w = std::clamp(cmd == KeyCmd::SizeUp ? w * 1.25f : w / 1.25f, 1.0f, tool_ == Tool::Text ? 200.0f : 96.0f);
        SaveSettings();
        EmitState();
        break;
      }
      case KeyCmd::Nudge: {
        POINT cur{};
        GetCursorPos(&cur);
        if (tracker_.Nudge(model_, cur, static_cast<double>(static_cast<int>(arg)))) {
          tracker_.Update(model_);
          InvalidateCaches();
        }
        break;
      }
      case KeyCmd::TextBackspace:
        if (live_.text && !live_.text->text.empty()) {
          live_.text->text.pop_back();
          Invalidate();
        }
        break;
      case KeyCmd::TextEnter:
        if (live_.text) {
          live_.text->text.push_back(L'\n');
          Invalidate();
        }
        break;
      case KeyCmd::TextCommit:
        CommitText();
        break;
      case KeyCmd::TextCancel:
        live_.text.reset();
        textEditing_ = false;
        Invalidate();
        break;
    }
  }

  // ---- hotkeys -----------------------------------------------------------------------

  void RegisterHotkeys() {
    for (auto &[id, _] : hotkeyIds_) UnregisterHotKey(controller_, id);
    hotkeyIds_.clear();
    hotkeyErrors_.clear();
    int id = 1;
    for (const auto &[action, text] : settings_.hotkeys) {
      if (text.empty()) continue;
      const auto hk = ParseHotkey(text);
      if (!hk || !RegisterHotKey(controller_, id, hk->modifiers | MOD_NOREPEAT, hk->vk)) {
        hotkeyErrors_.push_back(action); // usually taken by another app
        continue;
      }
      hotkeyIds_[id++] = action;
    }
  }

  void OnHotkey(int id) {
    auto it = hotkeyIds_.find(id);
    if (it == hotkeyIds_.end()) return;
    const std::string &a = it->second;
    if (a == "toggleDraw") SetActive(!active_);
    else if (a == "clear") ClearAll();
    else if (a == "toggleInk") SetInkVisible(!inkVisible_);
    else if (a == "screenshot") Screenshot();
    else if (a == "toggleMode") SetDocumentMode(!settings_.documentMode);
  }

  // ---- sync server -------------------------------------------------------------------

  void StartSyncServer() {
    sync_.Stop();
    SyncServer::Options opts;
    opts.port = settings_.syncPort;
    opts.allowedExtensionIds = settings_.allowedExtensionIds;
    const HWND controller = controller_;
    syncRunning_ = sync_.Start(opts, [controller](const std::string &m) {
      PostMessageW(controller, WM_APP_SYNC, 0, reinterpret_cast<LPARAM>(new std::string(m)));
    });
  }

  // ---- commands from JS --------------------------------------------------------------

  void SetTool(const std::string &name) {
    for (size_t i = 0; i < std::size(kTools); ++i) {
      if (name != kTools[i]) continue;
      if (live_.text && static_cast<Tool>(i) != Tool::Text) CommitText();
      tool_ = static_cast<Tool>(i);
      settings_.tool = name;
      if (tool_ != Tool::Eraser) live_.eraserCursor.reset();
      SaveSettings();
      Invalidate();
      EmitState();
      return;
    }
  }

  void SetBoard(Board b) {
    if (live_.text) CommitText();
    board_ = b;
    InvalidateCaches();
    // Picking a board implies you want to draw on it.
    if (b != Board::None && !active_) SetActive(true);
    EmitState();
  }

  void SetDocumentMode(bool on) {
    settings_.documentMode = on;
    SaveSettings();
    EmitState();
  }

  void SetInkVisible(bool v) {
    inkVisible_ = v;
    InvalidateCaches();
    EmitState();
  }

  void Undo() {
    if (live_.text) {
      live_.text.reset();
      textEditing_ = false;
    } else if (!model_.Undo()) {
      return;
    }
    InvalidateCaches();
    EmitState();
  }

  void Redo() {
    if (model_.Redo()) {
      InvalidateCaches();
      EmitState();
    }
  }

  void ClearAll() {
    live_.text.reset();
    textEditing_ = false;
    live_.laser.clear();
    // Clear what's visible: the board when a board is up, otherwise everything else.
    if (board_ != Board::None) model_.ClearLayer("board");
    else {
      for (const auto &l : model_.Layers())
        if (l->key != "board" && !l->items.empty()) model_.ClearLayer(l->key);
    }
    renderer_->ClearCache();
    InvalidateCaches();
    EmitState();
  }

  void ApplyDockSettings() {
    if (!dock_) return;
    SetWindowDisplayAffinity(dock_, settings_.hideDockFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    RaiseDock();
  }

  void HandleCommand(const std::string &text) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) return;
    const std::string cmd = j["cmd"];

    if (cmd == "getState") EmitState();
    else if (cmd == "setTool" && j.contains("tool") && j["tool"].is_string()) SetTool(j["tool"]);
    else if (cmd == "setActive" && j.contains("active") && j["active"].is_boolean()) SetActive(j["active"]);
    else if (cmd == "toggleActive") SetActive(!active_);
    else if (cmd == "setBoard") {
      const std::string b = j.value("board", "none");
      SetBoard(b == "white" ? Board::White : b == "black" ? Board::Black : Board::None);
    } else if (cmd == "setDocumentMode" && j.contains("enabled") && j["enabled"].is_boolean()) SetDocumentMode(j["enabled"]);
    else if (cmd == "setInkVisible" && j.contains("visible") && j["visible"].is_boolean()) SetInkVisible(j["visible"]);
    else if (cmd == "undo") Undo();
    else if (cmd == "redo") Redo();
    else if (cmd == "clear") ClearAll();
    else if (cmd == "screenshot") Screenshot();
    else if (cmd == "updateSettings" && j.contains("settings")) {
      const auto oldPort = settings_.syncPort;
      const auto oldIds = settings_.allowedExtensionIds;
      const auto oldHotkeys = settings_.hotkeys;
      settings_.Merge(j["settings"]);
      if (j["settings"].contains("tool")) SetTool(settings_.tool);
      live_.laserFadeSeconds = settings_.laserFadeSeconds;
      holdVk_ = ParseKeyName(settings_.clickThroughKey);
      if (oldHotkeys != settings_.hotkeys) RegisterHotkeys();
      if (oldPort != settings_.syncPort || oldIds != settings_.allowedExtensionIds) StartSyncServer();
      ApplyDockSettings();
      SaveSettings();
      Invalidate();
      EmitState();
    } else if (cmd == "resetSettings") {
      settings_ = Settings{};
      SetTool(settings_.tool);
      RegisterHotkeys();
      StartSyncServer();
      ApplyDockSettings();
      SaveSettings();
      EmitState();
    }
  }

  // ---- screenshot --------------------------------------------------------------------

  void Screenshot() {
    POINT cur{};
    GetCursorPos(&cur);
    const HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT r = mi.rcMonitor;
    const int w = r.right - r.left, h = r.bottom - r.top;

    // 1) Desktop pixels. Our overlay is a composition-only window, so GDI doesn't capture it.
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    {
      HDC screen = GetDC(nullptr);
      HDC mem = CreateCompatibleDC(screen);
      HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
      HGDIOBJ old = SelectObject(mem, bmp);
      BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
      SelectObject(mem, old);
      BITMAPINFO bi{};
      bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
      bi.bmiHeader.biWidth = w;
      bi.bmiHeader.biHeight = -h;
      bi.bmiHeader.biPlanes = 1;
      bi.bmiHeader.biBitCount = 32;
      GetDIBits(mem, bmp, 0, static_cast<UINT>(h), pixels.data(), &bi, DIB_RGB_COLORS);
      DeleteObject(bmp);
      DeleteDC(mem);
      ReleaseDC(nullptr, screen);
      for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    }

    // 2) Ink on top, via a WIC render target (CPU-side, still local).
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    ComPtr<IWICBitmap> bitmap;
    wic->CreateBitmapFromMemory(static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
                                static_cast<UINT>(w * 4), static_cast<UINT>(pixels.size()), pixels.data(), &bitmap);
    if (!bitmap) return;
    ComPtr<ID2D1RenderTarget> rt;
    const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                    96, 96);
    if (FAILED(device_.D2DFactory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return;
    {
      Renderer shotRenderer(device_);
      rt->BeginDraw();
      if (board_ != Board::None) Renderer::DrawBackground(rt.Get(), board_);
      rt->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(-r.left), static_cast<float>(-r.top)));
      if (inkVisible_) shotRenderer.DrawItems(rt.Get(), model_, live_, board_, Now());
      rt->EndDraw();
    }

    // 3) Save PNG to Pictures\ScrollPen and copy to the clipboard.
    PWSTR pics = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pics))) {
      std::wstring dir = std::wstring(pics) + L"\\ScrollPen";
      CoTaskMemFree(pics);
      CreateDirectoryW(dir.c_str(), nullptr);
      SYSTEMTIME st;
      GetLocalTime(&st);
      wchar_t name[64];
      swprintf_s(name, L"\\ScrollPen %04d-%02d-%02d %02d-%02d-%02d.png", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
      path = dir + name;
      ComPtr<IWICStream> stream;
      ComPtr<IWICBitmapEncoder> enc;
      ComPtr<IWICBitmapFrameEncode> frame;
      if (SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
          SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
          SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
          SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr))) {
        frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetPixelFormat(&fmt);
        frame->WriteSource(bitmap.Get(), nullptr);
        frame->Commit();
        enc->Commit();
      } else {
        path.clear();
      }
    }

    // Clipboard (CF_DIB, bottom-up 32-bit).
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    bitmap->CopyPixels(nullptr, static_cast<UINT>(w * 4), static_cast<UINT>(out.size()), out.data());
    if (OpenClipboard(controller_)) {
      EmptyClipboard();
      const size_t bytes = sizeof(BITMAPINFOHEADER) + out.size();
      if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        auto *bih = static_cast<BITMAPINFOHEADER *>(GlobalLock(g));
        *bih = {};
        bih->biSize = sizeof(BITMAPINFOHEADER);
        bih->biWidth = w;
        bih->biHeight = h; // bottom-up
        bih->biPlanes = 1;
        bih->biBitCount = 32;
        auto *dst = reinterpret_cast<uint8_t *>(bih + 1);
        for (int y = 0; y < h; ++y)
          memcpy(dst + static_cast<size_t>(h - 1 - y) * w * 4, out.data() + static_cast<size_t>(y) * w * 4,
                 static_cast<size_t>(w) * 4);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_DIB, g)) GlobalFree(g);
      }
      CloseClipboard();
    }
    Emit({{"ev", "screenshot"}, {"path", Narrow(path)}});
  }

  // ---- settings & events -------------------------------------------------------------

  void LoadSettings() {
    settingsPath_ = AppDataDir() + L"\\settings.json";
    if (auto text = ReadSmallFile(settingsPath_)) {
      const auto j = json::parse(*text, nullptr, false);
      if (!j.is_discarded()) settings_.Merge(j);
    }
    for (size_t i = 0; i < std::size(kTools); ++i)
      if (settings_.tool == kTools[i]) tool_ = static_cast<Tool>(i);
  }

  void SaveSettings() {
    if (!settingsPath_.empty()) WriteFileAtomic(settingsPath_, settings_.ToJson().dump(2));
  }

  void Emit(const json &j) {
    if (sink_) sink_(j.dump());
  }

  void EmitState() {
    const char *boards[] = {"none", "white", "black"};
    Emit({
        {"ev", "state"},
        {"active", active_},
        {"clickThroughHeld", holdDown_.load()},
        {"tool", kTools[static_cast<int>(tool_)]},
        {"board", boards[static_cast<int>(board_)]},
        {"inkVisible", inkVisible_},
        {"textEditing", textEditing_.load()},
        {"canUndo", model_.CanUndo() || live_.text.has_value()},
        {"canRedo", model_.CanRedo()},
        {"itemCount", model_.ItemCount()},
        {"sync", {{"source", lastSyncSource_}, {"confidence", tracker_.ActiveConfidence()}, {"serverRunning", syncRunning_}}},
        {"hotkeyErrors", hotkeyErrors_},
        {"hookReinstalls", hooks_.ReinstallCount()},
        {"monitors", overlays_.size()},
        {"settings", settings_.ToJson()},
    });
  }

  // ---- state -------------------------------------------------------------------------

  HWND dock_{};
  EventSink sink_;
  std::thread thread_;
  std::mutex startMutex_;
  std::condition_variable startCv_;
  bool started_{false};

  HWND controller_{};
  GraphicsDevice device_;
  std::unique_ptr<Renderer> renderer_;
  std::vector<Overlay> overlays_;
  bool needsRender_{false};
  bool clickThrough_{true};
  bool overlaysStyleDirty_{true};

  Model model_;
  LiveState live_;
  ScrollTracker tracker_;
  std::optional<ScrollTracker::Target> target_;
  std::string lastSyncSource_{"none"};
  std::map<std::string, std::vector<uint64_t>> erasing_;

  Settings settings_;
  std::wstring settingsPath_;
  Tool tool_{Tool::Pen};
  Tool strokeTool_{Tool::Pen};
  Board board_{Board::None};
  bool active_{false};
  bool inkVisible_{true};
  bool pointerDown_{false};
  UINT32 pointerId_{0};
  bool animating_{false};

  // Shared with the hook thread.
  std::atomic<bool> hooksDrawing_{false};
  std::atomic<bool> burst_{false};
  std::atomic<bool> holdDown_{false};
  std::atomic<bool> textEditing_{false};
  std::atomic<unsigned> holdVk_{VK_CAPITAL};
  bool swallowed_[256]{}; // hook thread only

  InputHooks hooks_;
  SyncServer sync_;
  bool syncRunning_{false};
  std::map<int, std::string> hotkeyIds_;
  std::vector<std::string> hotkeyErrors_;
};

Engine &Engine::Instance() {
  static Engine engine;
  return engine;
}

void Engine::Start(HWND dock, EventSink sink) {
  if (impl_) return;
  impl_ = std::make_unique<Impl>(dock, std::move(sink));
  impl_->Start();
}

void Engine::Stop() {
  if (!impl_) return;
  impl_->Stop();
  impl_.reset();
}

void Engine::Post(const std::string &commandJson) {
  if (impl_) impl_->Post(commandJson);
}

} // namespace sp
