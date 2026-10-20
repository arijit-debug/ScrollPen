#include "pch.h"
#include "Model.h"

namespace sp {

RectF Item::Bounds() const {
  if (points.empty()) return {};
  RectF r{points[0].x, points[0].y, points[0].x, points[0].y};
  for (const auto &p : points) {
    r.left = std::min(r.left, p.x);
    r.top = std::min(r.top, p.y);
    r.right = std::max(r.right, p.x);
    r.bottom = std::max(r.bottom, p.y);
  }
  if (kind == ItemKind::Text) {
    // Rough text box; the renderer measures precisely, this is only for hit-testing.
    size_t longest = 0, lines = 1, run = 0;
    for (wchar_t c : text) {
      if (c == L'\n') {
        ++lines;
        run = 0;
      } else {
        longest = std::max(longest, ++run);
      }
    }
    r.right = r.left + style.fontSize * 0.6f * static_cast<float>(std::max<size_t>(longest, 1));
    r.bottom = r.top + style.fontSize * 1.3f * static_cast<float>(lines);
  }
  return r.Inflated(style.width / 2);
}

PointF Layer::ToLayer(PointF s) const {
  if (!isDocument) return s;
  const auto &v = viewport;
  return {static_cast<float>((s.x - v.screenRect.left) / v.scale + v.scrollX),
          static_cast<float>((s.y - v.screenRect.top) / v.scale + v.scrollY)};
}

PointF Layer::ToScreen(PointF l) const {
  if (!isDocument) return l;
  const auto &v = viewport;
  return {static_cast<float>((l.x - v.scrollX) * v.scale + v.screenRect.left),
          static_cast<float>((l.y - v.scrollY) * v.scale + v.screenRect.top)};
}

namespace {

float DistSqToSegment(PointF p, PointF a, PointF b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  const float len2 = dx * dx + dy * dy;
  float t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
  t = std::clamp(t, 0.0f, 1.0f);
  const float cx = a.x + t * dx - p.x, cy = a.y + t * dy - p.y;
  return cx * cx + cy * cy;
}

} // namespace

bool ItemHit(const Item &item, const Layer &layer, PointF c, float radius) {
  if (item.points.empty()) return false;
  const float scale = layer.isDocument ? static_cast<float>(layer.viewport.scale) : 1.0f;
  const float reach = radius + item.style.width * scale / 2;

  auto S = [&](size_t i) { return layer.ToScreen({item.points[i].x, item.points[i].y}); };

  switch (item.kind) {
    case ItemKind::Stroke:
    case ItemKind::Highlight: {
      if (item.points.size() == 1) {
        const PointF a = S(0);
        return (a.x - c.x) * (a.x - c.x) + (a.y - c.y) * (a.y - c.y) <= reach * reach;
      }
      for (size_t i = 1; i < item.points.size(); ++i)
        if (DistSqToSegment(c, S(i - 1), S(i)) <= reach * reach) return true;
      return false;
    }
    case ItemKind::Line:
    case ItemKind::Arrow:
      return item.points.size() >= 2 && DistSqToSegment(c, S(0), S(1)) <= reach * reach;
    case ItemKind::Rect:
    case ItemKind::Ellipse:
    case ItemKind::Text: {
      const RectF b = item.Bounds();
      const PointF tl = layer.ToScreen({b.left, b.top});
      const PointF br = layer.ToScreen({b.right, b.bottom});
      const RectF sr{std::min(tl.x, br.x), std::min(tl.y, br.y), std::max(tl.x, br.x), std::max(tl.y, br.y)};
      if (item.kind == ItemKind::Text || item.style.fill) return sr.Inflated(radius).Contains(c);
      if (item.kind == ItemKind::Rect) {
        const PointF p1{sr.left, sr.top}, p2{sr.right, sr.top}, p3{sr.right, sr.bottom}, p4{sr.left, sr.bottom};
        return DistSqToSegment(c, p1, p2) <= reach * reach || DistSqToSegment(c, p2, p3) <= reach * reach ||
            DistSqToSegment(c, p3, p4) <= reach * reach || DistSqToSegment(c, p4, p1) <= reach * reach;
      }
      // Ellipse outline: compare normalised radius against 1 with a tolerance band.
      const float rx = sr.Width() / 2, ry = sr.Height() / 2;
      if (rx < 1 || ry < 1) return sr.Inflated(reach).Contains(c);
      const float nx = (c.x - (sr.left + rx)) / rx, ny = (c.y - (sr.top + ry)) / ry;
      const float d = std::sqrt(nx * nx + ny * ny);
      return std::abs(d - 1.0f) * std::min(rx, ry) <= reach;
    }
  }
  return false;
}

Model::Model() {
  auto screen = std::make_unique<Layer>();
  screen->key = "screen";
  layers_.push_back(std::move(screen));
}

Layer &Model::ScreenLayer() {
  return *layers_.front();
}

Layer *Model::FindLayer(const std::string &key) {
  for (auto &l : layers_)
    if (l->key == key) return l.get();
  return nullptr;
}

Layer &Model::GetOrCreateLayer(const std::string &key, bool isDocument) {
  if (auto *l = FindLayer(key)) return *l;
  auto layer = std::make_unique<Layer>();
  layer->key = key;
  layer->isDocument = isDocument;
  layers_.push_back(std::move(layer));
  return *layers_.back();
}

void Model::Apply(const Change &c, bool forward) {
  for (const auto &e : c.entries) {
    Layer *layer = FindLayer(e.layerKey);
    if (!layer) continue;
    const auto &toRemove = forward ? e.removed : e.added;
    const auto &toAdd = forward ? e.added : e.removed;
    auto &items = layer->items;
    for (const auto &r : toRemove)
      items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &i) { return i.id == r.id; }), items.end());
    for (const auto &a : toAdd) {
      // Keep z-order stable by id (ids increase with creation time).
      auto pos = std::upper_bound(items.begin(), items.end(), a.id, [](uint64_t id, const Item &i) { return id < i.id; });
      items.insert(pos, a);
    }
  }
}

void Model::Commit(Change c) {
  if (c.entries.empty()) return;
  Apply(c, true);
  undo_.push_back(std::move(c));
  if (undo_.size() > kMaxUndo) undo_.erase(undo_.begin());
  redo_.clear();
}

void Model::AddItem(const std::string &layerKey, Item item) {
  if (item.id == 0) item.id = NextId();
  Change c;
  c.entries.push_back({layerKey, {std::move(item)}, {}});
  Commit(std::move(c));
}

bool Model::RemoveItems(const std::string &layerKey, const std::vector<uint64_t> &ids) {
  Layer *layer = FindLayer(layerKey);
  if (!layer || ids.empty()) return false;
  Change::Entry e{layerKey, {}, {}};
  for (const auto &i : layer->items)
    if (std::find(ids.begin(), ids.end(), i.id) != ids.end()) e.removed.push_back(i);
  if (e.removed.empty()) return false;
  Change c;
  c.entries.push_back(std::move(e));
  Commit(std::move(c));
  return true;
}

void Model::ClearLayer(const std::string &layerKey) {
  Layer *layer = FindLayer(layerKey);
  if (!layer || layer->items.empty()) return;
  Change c;
  c.entries.push_back({layerKey, {}, layer->items});
  Commit(std::move(c));
}

void Model::ClearAll() {
  Change c;
  for (const auto &l : layers_)
    if (!l->items.empty()) c.entries.push_back({l->key, {}, l->items});
  Commit(std::move(c));
}

bool Model::Undo() {
  if (undo_.empty()) return false;
  Change c = std::move(undo_.back());
  undo_.pop_back();
  Apply(c, false);
  redo_.push_back(std::move(c));
  return true;
}

bool Model::Redo() {
  if (redo_.empty()) return false;
  Change c = std::move(redo_.back());
  redo_.pop_back();
  Apply(c, true);
  undo_.push_back(std::move(c));
  return true;
}

size_t Model::ItemCount() const {
  size_t n = 0;
  for (const auto &l : layers_) n += l->items.size();
  return n;
}

} // namespace sp
