#pragma once

#include <cstdint>
#include <vector>

namespace sp {

// Estimates how far a window's content scrolled vertically between two captures.
//
// Captures the target window itself (PrintWindow), so ScrollPen's own overlay is never
// in the image. Each row is reduced to a short luminance signature; the shift is found
// by a coarse search on every 4th row, then refined at full resolution.
class OpticalFlow {
 public:
  struct Result {
    bool valid{false};
    double dy{0};       // content moved up by dy px (positive = scrolled down)
    float confidence{0};
  };

  bool Prime(HWND topLevel, const RECT &region);
  Result Measure(HWND topLevel, const RECT &region);

  // Pure functions, exposed for unit tests. Signatures are rows x kCells bytes.
  static constexpr int kCells = 48;
  static Result EstimateShift(const std::vector<uint8_t> &prev, const std::vector<uint8_t> &cur, int rows);

 private:
  bool Capture(HWND topLevel, const RECT &region, std::vector<uint8_t> &sig, int &rows);

  std::vector<uint8_t> prev_;
  int rows_{0};
  RECT lastRegion_{};
};

} // namespace sp
