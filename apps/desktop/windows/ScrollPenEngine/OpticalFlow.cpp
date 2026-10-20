#include "pch.h"
#include "OpticalFlow.h"

namespace sp {

namespace {

// Mean absolute difference between rows a[r] and b[r + d] over the overlap, sampled every `step` rows.
double Cost(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, int rows, int d, int step) {
  const int cells = OpticalFlow::kCells;
  const int r0 = std::max(0, -d), r1 = std::min(rows, rows - d);
  if (r1 - r0 < rows / 4) return 1e9; // too little overlap to trust
  uint64_t sum = 0;
  int n = 0;
  for (int r = r0; r < r1; r += step) {
    const uint8_t *pa = &a[static_cast<size_t>(r) * cells];
    const uint8_t *pb = &b[static_cast<size_t>(r + d) * cells];
    for (int c = 0; c < cells; ++c) sum += static_cast<uint64_t>(std::abs(pa[c] - pb[c]));
    n += cells;
  }
  return n ? static_cast<double>(sum) / n : 1e9;
}

// Rows with structure (text, edges). Flat colour can't tell us anything.
double Texture(const std::vector<uint8_t> &s, int rows) {
  const int cells = OpticalFlow::kCells;
  uint64_t sum = 0;
  for (int r = 1; r < rows; ++r)
    for (int c = 0; c < cells; ++c)
      sum += static_cast<uint64_t>(std::abs(s[static_cast<size_t>(r) * cells + c] - s[static_cast<size_t>(r - 1) * cells + c]));
  return rows > 1 ? static_cast<double>(sum) / ((rows - 1) * cells) : 0;
}

} // namespace

OpticalFlow::Result OpticalFlow::EstimateShift(const std::vector<uint8_t> &prev, const std::vector<uint8_t> &cur,
                                               int rows) {
  Result res;
  if (rows < 32 || prev.size() != cur.size() || prev.size() != static_cast<size_t>(rows) * kCells) return res;
  if (Texture(cur, rows) < 0.6) return res; // blank region: no information

  // cur[r] shows what prev[r + dy] showed, so cost(d) compares cur[r] with prev[r + d].
  const int maxShift = rows * 3 / 4;
  double best = 1e18, second = 1e18;
  int bestD = 0;
  for (int d = -maxShift; d <= maxShift; d += 2) {
    const double c = Cost(cur, prev, rows, d, 4);
    if (c < best) {
      second = std::min(second, best);
      best = c;
      bestD = d;
    } else if (c < second && std::abs(d - bestD) > 6) {
      second = c;
    }
  }
  // Refine around the coarse optimum at full resolution.
  double fine = 1e18;
  int fineD = bestD;
  for (int d = bestD - 3; d <= bestD + 3; ++d) {
    const double c = Cost(cur, prev, rows, d, 1);
    if (c < fine) {
      fine = c;
      fineD = d;
    }
  }
  if (fine > 24) return res; // nothing matched well (content changed, video, zoom)

  res.valid = true;
  res.dy = fineD;
  // Distinct optimum => confident; repeating patterns (tables, ruled paper) => less so.
  const double ratio = second > 0 ? best / second : 1;
  res.confidence = static_cast<float>(std::clamp(1.0 - ratio, 0.2, 0.9));
  return res;
}

bool OpticalFlow::Capture(HWND topLevel, const RECT &region, std::vector<uint8_t> &sig, int &rows) {
  RECT wr{};
  GetWindowRect(topLevel, &wr);
  const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
  if (ww <= 0 || wh <= 0 || ww > 16384 || wh > 16384) return false;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = ww;
  bi.bmiHeader.biHeight = -wh;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  void *bits = nullptr;
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!bmp) {
    DeleteDC(mem);
    return false;
  }
  HGDIOBJ old = SelectObject(mem, bmp);
  const bool ok = PrintWindow(topLevel, mem, PW_RENDERFULLCONTENT) != FALSE;
  GdiFlush();

  if (ok) {
    // Central band of the region, up to 480 px wide, split into kCells cells per row.
    const int rx0 = std::clamp<int>(region.left - wr.left, 0, ww), rx1 = std::clamp<int>(region.right - wr.left, 0, ww);
    const int ry0 = std::clamp<int>(region.top - wr.top, 0, wh), ry1 = std::clamp<int>(region.bottom - wr.top, 0, wh);
    const int bandW = std::min(480, rx1 - rx0);
    const int bx0 = rx0 + ((rx1 - rx0) - bandW) / 2;
    rows = ry1 - ry0;
    const int cellW = std::max(1, bandW / kCells);
    sig.assign(static_cast<size_t>(std::max(rows, 0)) * kCells, 0);
    const auto *px = static_cast<const uint8_t *>(bits);
    for (int r = 0; r < rows; ++r) {
      const uint8_t *row = px + (static_cast<size_t>(ry0 + r) * ww) * 4;
      for (int c = 0; c < kCells; ++c) {
        unsigned acc = 0;
        int n = 0;
        for (int x = bx0 + c * cellW; x < bx0 + (c + 1) * cellW && x < rx1; ++x, ++n) {
          const uint8_t *p = row + static_cast<size_t>(x) * 4;
          acc += (p[2] * 77u + p[1] * 150u + p[0] * 29u) >> 8;
        }
        sig[static_cast<size_t>(r) * kCells + c] = static_cast<uint8_t>(n ? acc / n : 0);
      }
    }
  }

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  return ok && rows > 0;
}

bool OpticalFlow::Prime(HWND topLevel, const RECT &region) {
  lastRegion_ = region;
  return Capture(topLevel, region, prev_, rows_) && Texture(prev_, rows_) > 0.0;
}

OpticalFlow::Result OpticalFlow::Measure(HWND topLevel, const RECT &region) {
  std::vector<uint8_t> cur;
  int rows = 0;
  if (!Capture(topLevel, region, cur, rows)) return {};
  Result res;
  // A resized region means reflowed content: re-prime instead of guessing.
  if (rows == rows_ && region.right - region.left == lastRegion_.right - lastRegion_.left) {
    res = EstimateShift(prev_, cur, rows);
  }
  prev_ = std::move(cur);
  rows_ = rows;
  lastRegion_ = region;
  return res;
}

} // namespace sp
