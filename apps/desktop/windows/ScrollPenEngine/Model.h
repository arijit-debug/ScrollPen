#pragma once

// Annotation model: items live in layers; every mutation goes through Model so
// undo/redo stays consistent. Pure C++ (no Win32), unit-tested in ScrollPenEngine.Tests.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sp {

struct PointF {
  float x{};
  float y{};
};

struct InkPoint {
  float x{};
  float y{};
  float pressure{1.0f}; // 0..1
};

struct RectF {
  float left{}, top{}, right{}, bottom{};
  float Width() const { return right - left; }
  float Height() const { return bottom - top; }
  bool Contains(PointF p) const { return p.x >= left && p.x < right && p.y >= top && p.y < bottom; }
  bool Intersects(const RectF &o) const { return left < o.right && o.left < right && top < o.bottom && o.top < bottom; }
  RectF Inflated(float d) const { return {left - d, top - d, right + d, bottom + d}; }
};

enum class ItemKind : uint8_t { Stroke, Highlight, Line, Arrow, Rect, Ellipse, Text };

struct Style {
  uint32_t color{0xFFE53935}; // ARGB
  float width{4.0f};
  bool fill{false};
  float fontSize{28.0f};
};

// Coordinates are in the owning layer's space (see Layer).
struct Item {
  uint64_t id{};
  ItemKind kind{ItemKind::Stroke};
  Style style;
  std::vector<InkPoint> points; // stroke: all samples; shapes: [start, end]; text: [anchor]
  std::wstring text;
  RectF Bounds() const;
};

// Screen layer: coordinates are virtual-screen physical pixels.
// Document layer: coordinates are document units (CSS px for browsers, physical px otherwise);
// the renderer maps them to the screen with the layer's live Viewport.
struct Viewport {
  RectF screenRect;      // where the scrolled content is visible, physical px
  double scrollX{};      // current document scroll, document units
  double scrollY{};
  double scale{1.0};     // physical px per document unit
  bool visible{true};
  float confidence{1.0f}; // 0..1, how much we trust scrollX/Y
};

struct Layer {
  std::string key; // "screen" or a document key
  bool isDocument{false};
  Viewport viewport;
  std::vector<Item> items;

  PointF ToLayer(PointF screen) const;
  PointF ToScreen(PointF layer) const;
};

// Hit-tests an eraser circle (screen px) against an item already mapped to screen space.
bool ItemHit(const Item &item, const Layer &layer, PointF screenCenter, float radius);

class Model {
 public:
  Model();

  Layer &ScreenLayer();
  Layer *FindLayer(const std::string &key);
  Layer &GetOrCreateDocumentLayer(const std::string &key) { return GetOrCreateLayer(key, true); }
  Layer &GetOrCreateLayer(const std::string &key, bool isDocument);
  const std::vector<std::unique_ptr<Layer>> &Layers() const { return layers_; }

  uint64_t NextId() { return ++lastId_; }

  // Undoable operations.
  void AddItem(const std::string &layerKey, Item item);
  bool RemoveItems(const std::string &layerKey, const std::vector<uint64_t> &ids);
  void ClearLayer(const std::string &layerKey);
  void ClearAll();

  bool Undo();
  bool Redo();
  bool CanUndo() const { return !undo_.empty(); }
  bool CanRedo() const { return !redo_.empty(); }
  size_t ItemCount() const;

 private:
  struct Change {
    // Items removed from / added to each layer, applied atomically.
    struct Entry {
      std::string layerKey;
      std::vector<Item> added;
      std::vector<Item> removed;
    };
    std::vector<Entry> entries;
  };

  void Apply(const Change &c, bool forward);
  void Commit(Change c);

  std::vector<std::unique_ptr<Layer>> layers_;
  std::vector<Change> undo_;
  std::vector<Change> redo_;
  uint64_t lastId_{};
  static constexpr size_t kMaxUndo = 500;
};

} // namespace sp
