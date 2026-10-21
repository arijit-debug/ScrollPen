// Eraser hit tests, in layer units.
import type { Item } from './types';

function segDist(px: number, py: number, x1: number, y1: number, x2: number, y2: number): number {
  const dx = x2 - x1, dy = y2 - y1;
  const len2 = dx * dx + dy * dy;
  const t = len2 ? Math.max(0, Math.min(1, ((px - x1) * dx + (py - y1) * dy) / len2)) : 0;
  return Math.hypot(px - (x1 + t * dx), py - (y1 + t * dy));
}

function polylineDist(px: number, py: number, pts: number[], stride: number, closed = false): number {
  const n = pts.length / stride;
  let best = n ? Math.hypot(px - pts[0], py - pts[1]) : Infinity;
  for (let i = 1; i < n; i++)
    best = Math.min(best, segDist(px, py, pts[(i - 1) * stride], pts[(i - 1) * stride + 1], pts[i * stride], pts[i * stride + 1]));
  if (closed && n > 2)
    best = Math.min(best, segDist(px, py, pts[(n - 1) * stride], pts[(n - 1) * stride + 1], pts[0], pts[1]));
  return best;
}

/**
 * True when a circle of radius r at (x, y) touches the item. `box` is the rendered
 * bounding box, used for text.
 */
export function hitItem(item: Item, x: number, y: number, r: number, box?: DOMRect | null): boolean {
  const p = item.points;
  const reach = r + item.width / 2;
  switch (item.kind) {
    case 'pen':
    case 'highlighter':
      return polylineDist(x, y, p, 3) <= reach;
    case 'line':
    case 'arrow':
      return segDist(x, y, p[0], p[1], p[2], p[3]) <= reach;
    case 'rect': {
      const [x1, y1, x2, y2] = p;
      return polylineDist(x, y, [x1, y1, x2, y1, x2, y2, x1, y2], 2, true) <= reach;
    }
    case 'ellipse': {
      const cx = (p[0] + p[2]) / 2, cy = (p[1] + p[3]) / 2;
      const rx = Math.abs(p[2] - p[0]) / 2, ry = Math.abs(p[3] - p[1]) / 2;
      const ring: number[] = [];
      for (let i = 0; i < 48; i++) {
        const a = (i / 48) * Math.PI * 2;
        ring.push(cx + rx * Math.cos(a), cy + ry * Math.sin(a));
      }
      return polylineDist(x, y, ring, 2, true) <= reach;
    }
    case 'text': {
      if (!box) return false;
      return x >= box.x - r && x <= box.x + box.width + r && y >= box.y - r && y <= box.y + box.height + r;
    }
  }
}
