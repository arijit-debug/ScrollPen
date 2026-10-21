// Turns ink items into SVG elements. Everything is built with DOM calls (never markup
// strings), so text from a page or from storage can't become HTML.
import { getStroke } from 'perfect-freehand';
import type { Item } from './types';

export const SVG_NS = 'http://www.w3.org/2000/svg';
export const HIGHLIGHTER_OPACITY = 0.35;
export const TEXT_LINE_HEIGHT = 1.25;
export const TEXT_FONT = 'system-ui, "Segoe UI", Roboto, sans-serif';

export function svg<K extends keyof SVGElementTagNameMap>(tag: K, attrs: Record<string, string | number> = {}) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, String(v));
  return el;
}

const r2 = (n: number) => Math.round(n * 100) / 100;

/** Outline polygon to a smooth closed path (perfect-freehand's recommended form). */
function polygonPath(pts: number[][]): string {
  if (pts.length < 2) return '';
  const d: (string | number)[] = ['M', r2(pts[0][0]), r2(pts[0][1]), 'Q'];
  for (let i = 0; i < pts.length; i++) {
    const [x0, y0] = pts[i];
    const [x1, y1] = pts[(i + 1) % pts.length];
    d.push(r2(x0), r2(y0), r2((x0 + x1) / 2), r2((y0 + y1) / 2));
  }
  d.push('Z');
  return d.join(' ');
}

/** Polyline through x,y(,pressure) points, smoothed through segment midpoints. */
export function smoothPath(points: number[], stride: number): string {
  const n = points.length / stride;
  if (n === 1) return `M ${r2(points[0])} ${r2(points[1])} l 0.01 0`;
  let d = `M ${r2(points[0])} ${r2(points[1])}`;
  for (let i = 1; i < n - 1; i++) {
    const x = points[i * stride], y = points[i * stride + 1];
    const nx = points[(i + 1) * stride], ny = points[(i + 1) * stride + 1];
    d += ` Q ${r2(x)} ${r2(y)} ${r2((x + nx) / 2)} ${r2((y + ny) / 2)}`;
  }
  d += ` L ${r2(points[(n - 1) * stride])} ${r2(points[(n - 1) * stride + 1])}`;
  return d;
}

function penPath(item: Item, live: boolean): string {
  const pts: number[][] = [];
  for (let i = 0; i < item.points.length; i += 3) pts.push([item.points[i], item.points[i + 1], item.points[i + 2]]);
  const outline = getStroke(pts, {
    size: item.width,
    thinning: 0.55,
    smoothing: 0.5,
    streamline: 0.35,
    simulatePressure: false,
    last: !live,
  });
  return polygonPath(outline);
}

function arrowPath(x1: number, y1: number, x2: number, y2: number, width: number): string {
  const len = Math.hypot(x2 - x1, y2 - y1);
  const head = Math.min(len * 0.6, Math.max(12, width * 4));
  const a = Math.atan2(y2 - y1, x2 - x1);
  const spread = 0.5;
  const hx1 = x2 - head * Math.cos(a - spread), hy1 = y2 - head * Math.sin(a - spread);
  const hx2 = x2 - head * Math.cos(a + spread), hy2 = y2 - head * Math.sin(a + spread);
  return `M ${r2(x1)} ${r2(y1)} L ${r2(x2)} ${r2(y2)} M ${r2(hx1)} ${r2(hy1)} L ${r2(x2)} ${r2(y2)} L ${r2(hx2)} ${r2(hy2)}`;
}

export const FILL_OPACITY = 0.35;

const strokeAttrs = (item: Item) => ({
  fill: item.fill ? item.color : 'none',
  ...(item.fill ? { 'fill-opacity': FILL_OPACITY } : {}),
  stroke: item.color,
  'stroke-width': r2(item.width),
  'stroke-linecap': 'round',
  'stroke-linejoin': 'round',
});

/** The SVG element for an item (`live` while it is still being drawn). */
export function renderItem(item: Item, live = false): SVGGraphicsElement {
  const p = item.points;
  let el: SVGGraphicsElement;
  switch (item.kind) {
    case 'pen':
      el = svg('path', { d: penPath(item, live), fill: item.color });
      break;
    case 'highlighter':
      el = svg('path', { d: smoothPath(p, 3), ...strokeAttrs(item), 'stroke-opacity': HIGHLIGHTER_OPACITY });
      break;
    case 'line':
      el = svg('path', { d: `M ${r2(p[0])} ${r2(p[1])} L ${r2(p[2])} ${r2(p[3])}`, ...strokeAttrs(item) });
      break;
    case 'arrow':
      el = svg('path', { d: arrowPath(p[0], p[1], p[2], p[3], item.width), ...strokeAttrs(item) });
      break;
    case 'rect':
      el = svg('rect', {
        x: r2(Math.min(p[0], p[2])),
        y: r2(Math.min(p[1], p[3])),
        width: r2(Math.abs(p[2] - p[0])),
        height: r2(Math.abs(p[3] - p[1])),
        ...strokeAttrs(item),
      });
      break;
    case 'ellipse':
      el = svg('ellipse', {
        cx: r2((p[0] + p[2]) / 2),
        cy: r2((p[1] + p[3]) / 2),
        rx: r2(Math.abs(p[2] - p[0]) / 2),
        ry: r2(Math.abs(p[3] - p[1]) / 2),
        ...strokeAttrs(item),
      });
      break;
    case 'text': {
      const t = svg('text', { x: r2(p[0]), y: r2(p[1]), fill: item.color, 'font-size': r2(item.width) });
      t.style.fontFamily = TEXT_FONT;
      t.style.whiteSpace = 'pre';
      (item.text ?? '').split('\n').forEach((line, i) => {
        const span = svg('tspan', { x: r2(p[0]), dy: i === 0 ? '1.02em' : `${TEXT_LINE_HEIGHT}em` });
        span.textContent = line || ' ';
        t.append(span);
      });
      el = t;
      break;
    }
  }
  el.dataset.id = item.id;
  return el;
}
