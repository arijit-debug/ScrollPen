export const TOOLS = ['pen', 'highlighter', 'laser', 'eraser', 'line', 'arrow', 'rect', 'ellipse', 'text'] as const;
export type Tool = (typeof TOOLS)[number];

/** What a saved item is. Laser and eraser leave nothing behind. */
export type Kind = 'pen' | 'highlighter' | 'line' | 'arrow' | 'rect' | 'ellipse' | 'text';
const KINDS: readonly Kind[] = ['pen', 'highlighter', 'line', 'arrow', 'rect', 'ellipse', 'text'];

/**
 * One piece of ink. Coordinates are in its layer's units:
 * - "doc": CSS px from the top-left of the document (moves with the page's scroll)
 * - "el:<css path>": CSS px inside an inner scrolling element's content
 * - "image": pixels of the picture shown in an image tab
 * - "board": CSS px of the tab's viewport (the whiteboard doesn't scroll)
 * - "screen": CSS px of the tab's viewport, over the page ("follow scrolling" off)
 */
export interface Item {
  id: string;
  kind: Kind;
  layer: string;
  color: string;
  /** Stroke width, or font size for text, in layer units. */
  width: number;
  /** pen/highlighter: x,y,pressure triples; shapes: x1,y1,x2,y2; text: left,top. */
  points: number[];
  text?: string;
  /** Rectangles and ellipses: filled with a see-through tint of the colour. */
  fill?: boolean;
}

export type SizeKey = 'pen' | 'highlighter' | 'shape' | 'text';

/** Keys that can be held to click and type on the page without leaving the pen. */
export const CLICK_THROUGH_KEYS = ['CapsLock', '`', 'F8', 'RCtrl', ''] as const;
export type ClickThroughKey = (typeof CLICK_THROUGH_KEYS)[number];

/**
 * The user's answer to "save my ink on this computer?", asked in the product before
 * anything is stored (Chrome Web Store user data policy):
 * - ask: not answered yet; nothing is saved
 * - save: ink, page address and title are kept in this Chrome profile
 * - tab: ink lives only in the open tab and is gone when it closes; nothing is saved
 */
export const INK_STORAGE = ['ask', 'save', 'tab'] as const;
export type InkStorage = (typeof INK_STORAGE)[number];

export interface Settings {
  inkStorage: InkStorage;
  tool: Tool;
  color: string;
  palette: string[];
  sizes: Record<SizeKey, number>;
  /** Filled rectangles and ellipses. */
  fill: boolean;
  laserFadeSeconds: number;
  /** Ink sticks to the page as it scrolls; off: ink stays put on the screen. */
  documentMode: boolean;
  clickThroughKey: ClickThroughKey;
  /** Show the toolbar on every page as it opens. */
  startActive: boolean;
  /** Open PDFs in ScrollPen's viewer, where ink can be drawn on them. */
  openPdfs: boolean;
  /** Toolbar position (left, top in CSS px), or null for the default corner. */
  toolbar: { x: number; y: number } | null;
  toolbarMinimized: boolean;
}

export const DEFAULT_SETTINGS: Settings = {
  inkStorage: 'ask',
  tool: 'pen',
  color: '#E53935',
  palette: ['#E53935', '#FB8C00', '#FDD835', '#43A047', '#1E88E5', '#8E24AA', '#212121', '#FFFFFF', '#00ACC1'],
  sizes: { pen: 4, highlighter: 22, shape: 4, text: 28 },
  fill: false,
  laserFadeSeconds: 1.2,
  documentMode: true,
  clickThroughKey: 'CapsLock',
  startActive: false,
  openPdfs: true,
  toolbar: null,
  toolbarMinimized: false,
};

export const SIZE_RANGE: Record<SizeKey, [number, number]> = {
  pen: [1, 96],
  highlighter: [4, 96],
  shape: [1, 96],
  text: [8, 200],
};
export const LASER_FADE_RANGE: [number, number] = [0.2, 10];

export function sizeKey(tool: Tool): SizeKey {
  if (tool === 'highlighter') return 'highlighter';
  if (tool === 'text') return 'text';
  if (tool === 'pen' || tool === 'laser' || tool === 'eraser') return 'pen';
  return 'shape';
}

const COLOR = /^#[0-9a-fA-F]{6}$/;
export const isColor = (v: unknown): v is string => typeof v === 'string' && COLOR.test(v);
const finite = (v: unknown): v is number => typeof v === 'number' && Number.isFinite(v);
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));

/** Settings from storage, with every field checked and anything invalid replaced by its default. */
export function sanitizeSettings(raw: unknown): Settings {
  const s = (raw && typeof raw === 'object' ? raw : {}) as Partial<Record<keyof Settings, unknown>>;
  const d = DEFAULT_SETTINGS;
  const palette = Array.isArray(s.palette) ? s.palette.filter(isColor).slice(0, 12) : [];
  const sizesIn = (s.sizes && typeof s.sizes === 'object' ? s.sizes : {}) as Record<string, unknown>;
  const sizes = { ...d.sizes };
  for (const k of Object.keys(sizes) as SizeKey[]) {
    const v = sizesIn[k];
    if (finite(v)) sizes[k] = clamp(v, ...SIZE_RANGE[k]);
  }
  const tb = s.toolbar as { x?: unknown; y?: unknown } | null | undefined;
  const bool = (k: 'fill' | 'documentMode' | 'startActive' | 'openPdfs' | 'toolbarMinimized') =>
    typeof s[k] === 'boolean' ? (s[k] as boolean) : d[k];
  return {
    inkStorage: INK_STORAGE.includes(s.inkStorage as InkStorage) ? (s.inkStorage as InkStorage) : d.inkStorage,
    tool: TOOLS.includes(s.tool as Tool) ? (s.tool as Tool) : d.tool,
    color: isColor(s.color) ? s.color : d.color,
    palette: palette.length ? palette : d.palette,
    sizes,
    fill: bool('fill'),
    laserFadeSeconds: finite(s.laserFadeSeconds) ? clamp(s.laserFadeSeconds, ...LASER_FADE_RANGE) : d.laserFadeSeconds,
    documentMode: bool('documentMode'),
    clickThroughKey: CLICK_THROUGH_KEYS.includes(s.clickThroughKey as ClickThroughKey)
      ? (s.clickThroughKey as ClickThroughKey)
      : d.clickThroughKey,
    startActive: bool('startActive'),
    openPdfs: bool('openPdfs'),
    toolbar: tb && finite(tb.x) && finite(tb.y) ? { x: tb.x, y: tb.y } : null,
    toolbarMinimized: bool('toolbarMinimized'),
  };
}

const MAX_ITEMS = 20_000;
const MAX_POINTS = 60_000;
const MAX_TEXT = 5_000;
const LAYER = /^(doc|image|board|screen|el:.{1,2048})$/s;

/** Saved items, keeping only well-formed ones (storage is ours, but never trust it blindly). */
export function sanitizeItems(raw: unknown): Item[] {
  if (!Array.isArray(raw)) return [];
  const out: Item[] = [];
  for (const r of raw.slice(0, MAX_ITEMS)) {
    if (!r || typeof r !== 'object') continue;
    const i = r as Record<string, unknown>;
    if (typeof i.id !== 'string' || i.id.length > 64) continue;
    if (!KINDS.includes(i.kind as Kind) || typeof i.layer !== 'string' || !LAYER.test(i.layer)) continue;
    if (!isColor(i.color) || !finite(i.width) || i.width <= 0 || i.width > 10_000) continue;
    if (!Array.isArray(i.points) || i.points.length > MAX_POINTS || !i.points.every(finite)) continue;
    const kind = i.kind as Kind;
    const n = i.points.length;
    if (kind === 'pen' || kind === 'highlighter' ? n < 3 || n % 3 !== 0 : kind === 'text' ? n !== 2 : n !== 4) continue;
    if (kind === 'text' && (typeof i.text !== 'string' || !i.text || i.text.length > MAX_TEXT)) continue;
    const item: Item = { id: i.id, kind, layer: i.layer, color: i.color, width: i.width, points: i.points as number[] };
    if (kind === 'text') item.text = i.text as string;
    if ((kind === 'rect' || kind === 'ellipse') && i.fill === true) item.fill = true;
    out.push(item);
  }
  return out;
}

let counter = 0;
export function newId(): string {
  counter = (counter + 1) % 1e6;
  return `${Date.now().toString(36)}${counter.toString(36)}${Math.random().toString(36).slice(2, 7)}`;
}
