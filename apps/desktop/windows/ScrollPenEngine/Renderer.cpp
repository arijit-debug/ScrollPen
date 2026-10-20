#include "pch.h"
#include "Renderer.h"

namespace sp {

namespace {

D2D1_COLOR_F ToColor(uint32_t argb, float opacity = 1.0f) {
  return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                      ((argb >> 24) & 0xFF) / 255.0f * opacity);
}

bool HasPressure(const Item &item) {
  for (const auto &p : item.points)
    if (p.pressure < 0.999f) return true;
  return false;
}

constexpr float kHighlightOpacity = 0.38f;

} // namespace

bool GraphicsDevice::Create() {
  Reset();
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                      D3D_FEATURE_LEVEL_10_0};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
  if (FAILED(hr)) {
    // No usable GPU driver (e.g. inside some VMs): WARP still runs locally on the CPU.
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                           D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
  }
  if (FAILED(hr)) return false;
  if (FAILED(d3d_.As(&dxgiDevice_))) return false;
  if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory_)))) return false;

  D2D1_FACTORY_OPTIONS opts{};
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &opts,
                               reinterpret_cast<void **>(d2dFactory_.GetAddressOf()))))
    return false;
  if (FAILED(d2dFactory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_))) return false;
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf()))))
    return false;
  if (FAILED(DCompositionCreateDevice(dxgiDevice_.Get(), IID_PPV_ARGS(&dcomp_)))) return false;
  return true;
}

void GraphicsDevice::Reset() {
  dcomp_.Reset();
  dwrite_.Reset();
  d2dDevice_.Reset();
  d2dFactory_.Reset();
  dxgiFactory_.Reset();
  dxgiDevice_.Reset();
  d3d_.Reset();
}

bool GraphicsDevice::IsLost() const {
  return !d3d_ || FAILED(d3d_->GetDeviceRemovedReason());
}

ID2D1SolidColorBrush *Renderer::Brush(ID2D1RenderTarget *rt, uint32_t argb, float opacity) {
  if (!brush_ || brushOwner_ != rt) {
    brush_.Reset();
    rt->CreateSolidColorBrush(ToColor(argb, opacity), &brush_);
    brushOwner_ = rt;
  } else {
    brush_->SetColor(ToColor(argb, opacity));
  }
  return brush_.Get();
}

ID2D1StrokeStyle *Renderer::RoundStyle() {
  if (!round_) {
    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    device_.D2DFactory()->CreateStrokeStyle(props, nullptr, 0, &round_);
  }
  return round_.Get();
}

IDWriteTextFormat *Renderer::Font(float size) {
  const int key = static_cast<int>(std::lround(size * 4));
  auto it = fonts_.find(key);
  if (it != fonts_.end()) return it->second.Get();
  ComPtr<IDWriteTextFormat> f;
  device_.DWrite()->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
  if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return (fonts_[key] = f).Get();
}

void Renderer::DrawBackground(ID2D1RenderTarget *rt, Board board) {
  switch (board) {
    case Board::None:
      rt->Clear(D2D1::ColorF(0, 0, 0, 0));
      break;
    case Board::White:
      rt->Clear(D2D1::ColorF(0.98f, 0.98f, 0.97f, 1.0f));
      break;
    case Board::Black:
      rt->Clear(D2D1::ColorF(0.11f, 0.13f, 0.12f, 1.0f));
      break;
  }
}

bool Renderer::LaserActive(const LiveState &live, double now) {
  for (const auto &trail : live.laser)
    if (!trail.empty() && now - trail.back().t < live.laserFadeSeconds) return true;
  return false;
}

void Renderer::DrawCommitted(ID2D1RenderTarget *rt, const Model &model, const LiveState &live, Board board) {
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
  D2D1_MATRIX_3X2_F base;
  rt->GetTransform(&base);

  for (const auto &layer : model.Layers()) {
    // On a board only the board layer shows; otherwise everything except the board.
    const bool isBoard = layer->key == "board";
    if ((board != Board::None) != isBoard) continue;
    if (layer->isDocument && !layer->viewport.visible) continue;
    if (layer->isDocument) {
      const auto &v = layer->viewport;
      rt->PushAxisAlignedClip(D2D1::RectF(v.screenRect.left, v.screenRect.top, v.screenRect.right, v.screenRect.bottom),
                              D2D1_ANTIALIAS_MODE_ALIASED);
      const auto s = static_cast<float>(v.scale);
      rt->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(-v.scrollX), static_cast<float>(-v.scrollY)) *
                       D2D1::Matrix3x2F::Scale(s, s) *
                       D2D1::Matrix3x2F::Translation(v.screenRect.left, v.screenRect.top) * base);
      DrawLayer(rt, *layer, live);
      rt->SetTransform(base);
      rt->PopAxisAlignedClip();
    } else {
      DrawLayer(rt, *layer, live);
    }
  }
}

void Renderer::DrawLive(ID2D1RenderTarget *rt, const LiveState &live, Board board, double now) {
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  // Live items are always in screen coordinates.
  if (live.active) DrawItem(rt, *live.active, nullptr);
  if (live.text) DrawItem(rt, *live.text, nullptr, live.caretOn);
  DrawLaser(rt, live, now);
  if (live.eraserCursor) {
    auto *b = Brush(rt, board == Board::Black ? 0xFFFFFFFF : 0xFF333333, 0.8f);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(live.eraserCursor->x, live.eraserCursor->y), live.eraserRadius,
                                  live.eraserRadius),
                    b, 1.5f);
  }
}

void Renderer::DrawLayer(ID2D1RenderTarget *rt, const Layer &layer, const LiveState &live) {
  for (const auto &item : layer.items) {
    if (!live.hidden.empty() && std::find(live.hidden.begin(), live.hidden.end(), item.id) != live.hidden.end())
      continue;
    DrawItem(rt, item, &layer);
  }
}

void Renderer::DrawStroke(ID2D1RenderTarget *rt, const Item &item, const Layer *layer) {
  const bool highlight = item.kind == ItemKind::Highlight;
  auto *brush = Brush(rt, item.style.color, highlight ? kHighlightOpacity : 1.0f);
  const auto &pts = item.points;
  if (pts.size() == 1) {
    const float r = item.style.width * (highlight ? 1.0f : std::max(0.25f, pts[0].pressure)) / 2;
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(pts[0].x, pts[0].y), r, r), brush);
    return;
  }

  // Pen with real pressure: per-segment widths (not cached; still cheap on the GPU).
  if (!highlight && HasPressure(item)) {
    for (size_t i = 1; i < pts.size(); ++i) {
      const float p = (pts[i - 1].pressure + pts[i].pressure) / 2;
      rt->DrawLine(D2D1::Point2F(pts[i - 1].x, pts[i - 1].y), D2D1::Point2F(pts[i].x, pts[i].y), brush,
                   item.style.width * std::max(0.2f, p), RoundStyle());
    }
    return;
  }

  // Uniform width: one smoothed path (quadratic curves through segment midpoints).
  ComPtr<ID2D1PathGeometry> geo;
  const bool cacheable = layer != nullptr && item.id != 0;
  if (cacheable) {
    auto it = geometry_.find(item.id);
    if (it != geometry_.end()) geo = it->second;
  }
  if (!geo) {
    device_.D2DFactory()->CreatePathGeometry(&geo);
    ComPtr<ID2D1GeometrySink> sink;
    geo->Open(&sink);
    sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
      const D2D1_POINT_2F mid = D2D1::Point2F((pts[i].x + pts[i + 1].x) / 2, (pts[i].y + pts[i + 1].y) / 2);
      sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(pts[i].x, pts[i].y), mid));
    }
    sink->AddLine(D2D1::Point2F(pts.back().x, pts.back().y));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    if (cacheable) geometry_[item.id] = geo;
  }
  rt->DrawGeometry(geo.Get(), brush, item.style.width, RoundStyle());
}

void Renderer::DrawItem(ID2D1RenderTarget *rt, const Item &item, const Layer *layer, bool caret) {
  if (item.points.empty()) return;
  const auto &s = item.style;
  switch (item.kind) {
    case ItemKind::Stroke:
    case ItemKind::Highlight:
      DrawStroke(rt, item, layer);
      return;
    default:
      break;
  }

  auto *brush = Brush(rt, s.color);
  const D2D1_POINT_2F a = D2D1::Point2F(item.points[0].x, item.points[0].y);
  const D2D1_POINT_2F b = item.points.size() > 1 ? D2D1::Point2F(item.points[1].x, item.points[1].y) : a;

  switch (item.kind) {
    case ItemKind::Line:
      rt->DrawLine(a, b, brush, s.width, RoundStyle());
      break;
    case ItemKind::Arrow: {
      const float dx = b.x - a.x, dy = b.y - a.y;
      const float len = std::sqrt(dx * dx + dy * dy);
      if (len < 0.5f) break;
      const float ux = dx / len, uy = dy / len;
      const float head = std::min(len * 0.6f, 10.0f + s.width * 3.0f);
      const float half = head * 0.55f;
      const D2D1_POINT_2F base = D2D1::Point2F(b.x - ux * head, b.y - uy * head);
      rt->DrawLine(a, D2D1::Point2F(base.x + ux * 1.0f, base.y + uy * 1.0f), brush, s.width, RoundStyle());
      ComPtr<ID2D1PathGeometry> tri;
      device_.D2DFactory()->CreatePathGeometry(&tri);
      ComPtr<ID2D1GeometrySink> sink;
      tri->Open(&sink);
      sink->BeginFigure(b, D2D1_FIGURE_BEGIN_FILLED);
      sink->AddLine(D2D1::Point2F(base.x - uy * half, base.y + ux * half));
      sink->AddLine(D2D1::Point2F(base.x + uy * half, base.y - ux * half));
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      rt->FillGeometry(tri.Get(), brush);
      rt->DrawGeometry(tri.Get(), brush, std::max(1.0f, s.width * 0.5f), RoundStyle());
      break;
    }
    case ItemKind::Rect: {
      const D2D1_RECT_F r = D2D1::RectF(std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y));
      if (s.fill) rt->FillRectangle(r, Brush(rt, s.color, 0.35f));
      rt->DrawRectangle(r, Brush(rt, s.color), s.width, RoundStyle());
      break;
    }
    case ItemKind::Ellipse: {
      const D2D1_ELLIPSE e =
          D2D1::Ellipse(D2D1::Point2F((a.x + b.x) / 2, (a.y + b.y) / 2), std::abs(b.x - a.x) / 2, std::abs(b.y - a.y) / 2);
      if (s.fill) rt->FillEllipse(e, Brush(rt, s.color, 0.35f));
      rt->DrawEllipse(e, Brush(rt, s.color), s.width);
      break;
    }
    case ItemKind::Text: {
      auto *fmt = Font(s.fontSize);
      if (!fmt) break;
      ComPtr<IDWriteTextLayout> tl;
      const std::wstring &t = item.text;
      device_.DWrite()->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), fmt, 10000.0f, 10000.0f, &tl);
      if (!tl) break;
      rt->DrawTextLayout(a, tl.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
      if (caret) {
        DWRITE_HIT_TEST_METRICS m{};
        FLOAT cx{}, cy{};
        tl->HitTestTextPosition(static_cast<UINT32>(t.size()), FALSE, &cx, &cy, &m);
        const float h = m.height > 0 ? m.height : s.fontSize * 1.3f;
        rt->DrawLine(D2D1::Point2F(a.x + cx + 1, a.y + cy), D2D1::Point2F(a.x + cx + 1, a.y + cy + h), brush, 2.0f);
      }
      break;
    }
    default:
      break;
  }
}

void Renderer::DrawLaser(ID2D1RenderTarget *rt, const LiveState &live, double now) {
  for (const auto &trail : live.laser) {
    for (size_t i = 1; i < trail.size(); ++i) {
      const double age = now - trail[i].t;
      const float alpha = static_cast<float>(1.0 - age / live.laserFadeSeconds);
      if (alpha <= 0) continue;
      // Glow underneath, bright core on top.
      const auto a = D2D1::Point2F(trail[i - 1].p.x, trail[i - 1].p.y);
      const auto b = D2D1::Point2F(trail[i].p.x, trail[i].p.y);
      rt->DrawLine(a, b, Brush(rt, 0xFFFF1744, alpha * 0.35f), 14.0f, RoundStyle());
      rt->DrawLine(a, b, Brush(rt, 0xFFFF5252, alpha), 5.0f, RoundStyle());
    }
    if (trail.size() == 1 && now - trail[0].t < live.laserFadeSeconds) {
      const auto c = D2D1::Point2F(trail[0].p.x, trail[0].p.y);
      rt->FillEllipse(D2D1::Ellipse(c, 6, 6), Brush(rt, 0xFFFF5252));
    }
  }
}

} // namespace sp
