#pragma once

#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Model.h"

namespace sp {

using Microsoft::WRL::ComPtr;

// One D3D11/D2D/DWrite/DComp device shared by every overlay window. All drawing
// happens on the local GPU; nothing is rendered remotely.
class GraphicsDevice {
 public:
  bool Create();
  void Reset();
  bool IsLost() const;

  ID3D11Device *D3D() const { return d3d_.Get(); }
  IDXGIFactory2 *DxgiFactory() const { return dxgiFactory_.Get(); }
  ID2D1Factory3 *D2DFactory() const { return d2dFactory_.Get(); }
  ID2D1Device2 *D2D() const { return d2dDevice_.Get(); }
  IDWriteFactory *DWrite() const { return dwrite_.Get(); }
  IDCompositionDevice *DComp() const { return dcomp_.Get(); }

 private:
  ComPtr<ID3D11Device> d3d_;
  ComPtr<IDXGIDevice> dxgiDevice_;
  ComPtr<IDXGIFactory2> dxgiFactory_;
  ComPtr<ID2D1Factory3> d2dFactory_;
  ComPtr<ID2D1Device2> d2dDevice_;
  ComPtr<IDWriteFactory> dwrite_;
  ComPtr<IDCompositionDevice> dcomp_;
};

enum class Board : uint8_t { None, White, Black };

// A fading laser trail point.
struct LaserPoint {
  PointF p;
  double t; // seconds (steady clock)
};

// Everything that is drawn but not (yet) part of the model.
struct LiveState {
  std::optional<Item> active; // shape or stroke being drawn, in screen coordinates
  std::vector<std::vector<LaserPoint>> laser; // finished + current laser trails
  double laserFadeSeconds{1.2};
  std::vector<uint64_t> hidden; // items under the eraser, pending removal
  std::optional<PointF> eraserCursor;
  float eraserRadius{14};
  // Text being typed.
  std::optional<Item> text;
  bool caretOn{true};
  // A short message next to the pointer (e.g. "open this in Chrome"), fading out.
  struct Notice {
    std::wstring text;
    PointF at;
    double shownAt{};
  };
  std::optional<Notice> notice;
  static constexpr double kNoticeSeconds = 2.5;
};

class Renderer {
 public:
  explicit Renderer(GraphicsDevice &device) : device_(device) {}

  // All Draw* calls expect the target's transform to map virtual-screen px to the target.
  static void DrawBackground(ID2D1RenderTarget *rt, Board board);
  // Finished items (what the overlays cache as a bitmap).
  void DrawCommitted(ID2D1RenderTarget *rt, const Model &model, const LiveState &live, Board board);
  // In-progress stroke/shape, text being typed, laser trails, eraser cursor.
  void DrawLive(ID2D1RenderTarget *rt, const LiveState &live, Board board, double now);
  // Both, used for screenshots.
  void DrawItems(ID2D1RenderTarget *rt, const Model &model, const LiveState &live, Board board, double now) {
    DrawCommitted(rt, model, live, board);
    DrawLive(rt, live, board, now);
  }
  static bool LaserActive(const LiveState &live, double now);
  static bool NoticeActive(const LiveState &live, double now) {
    return live.notice && now - live.notice->shownAt < LiveState::kNoticeSeconds;
  }

  void ClearCache() { geometry_.clear(); }
  void Forget(uint64_t id) { geometry_.erase(id); }

 private:
  void DrawLayer(ID2D1RenderTarget *rt, const Layer &layer, const LiveState &live);
  void DrawItem(ID2D1RenderTarget *rt, const Item &item, const Layer *layer, bool caret = false);
  void DrawStroke(ID2D1RenderTarget *rt, const Item &item, const Layer *layer);
  void DrawLaser(ID2D1RenderTarget *rt, const LiveState &live, double now);
  void DrawNotice(ID2D1RenderTarget *rt, const LiveState &live, double now);
  ID2D1SolidColorBrush *Brush(ID2D1RenderTarget *rt, uint32_t argb, float opacity = 1.0f);
  IDWriteTextFormat *Font(float size);
  ID2D1StrokeStyle *RoundStyle();

  GraphicsDevice &device_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ID2D1RenderTarget *brushOwner_{};
  ComPtr<ID2D1StrokeStyle> round_;
  std::map<int, ComPtr<IDWriteTextFormat>> fonts_;
  // Smoothed path geometry for finished strokes, keyed by item id.
  std::unordered_map<uint64_t, ComPtr<ID2D1PathGeometry>> geometry_;
};

} // namespace sp
