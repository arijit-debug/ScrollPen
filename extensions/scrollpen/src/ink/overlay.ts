// ScrollPen's drawing layer for one page (or for ScrollPen's PDF viewer).
//
// Everything lives in one element appended to <html>, with a closed shadow root so the
// page's scripts and CSS can't reach in. Ink on the page itself is an SVG positioned in
// document coordinates, so the browser scrolls and zooms it together with the content
// (no lag, nothing to keep in sync). Ink on an inner scrolling element (a chat pane, a
// code viewer) is anchored to that element and follows its scroll.
import { hitItem } from './geometry';
import { icon, type IconName } from './icons';
import { renderItem, svg } from './render';
import { inkKey, loadInk, loadSettings, sanitizeSaved, saveInk, saveSettings, SETTINGS_KEY } from './storage';
import { HOST_STYLE, SHADOW_CSS } from './styles';
import {
  newId,
  sanitizeSettings,
  SIZE_RANGE,
  sizeKey,
  DEFAULT_SETTINGS,
  type Item,
  type Kind,
  type Settings,
  type Tool,
} from './types';

const HOST_TAG = 'scrollpen-ink';
const ERASER_RADIUS = 10;
const SAVE_DELAY_MS = 300;
const MAX_UNDO = 200;
const TOOLBAR_WIDTH = 44;
const SIZE_STEP = 1.25;

type LayerKind = 'doc' | 'el' | 'image' | 'board' | 'screen';
interface Layer {
  key: string;
  kind: LayerKind;
  svg: SVGSVGElement;
  g: SVGGElement;
  target: Element | null;
  /** Layer units per CSS px on screen (only image layers differ from 1). */
  scale: number;
}

/** One undoable change: these items were removed and those added. Undo swaps them. */
interface Op {
  add: Item[];
  remove: Item[];
}

type Drag =
  | { tool: 'laser' }
  | { tool: 'place-text'; x: number; y: number }
  | { tool: 'eraser'; removed: Item[] }
  | { tool: Kind; item: Item; layer: Layer; el: SVGGraphicsElement; sx: number; sy: number };

interface Editor {
  ta: HTMLTextAreaElement;
  layer: Layer;
  at: [number, number];
  color: string;
  size: number;
}

const TOOL_BUTTONS: { tool: Tool; icon: IconName; label: string; key: string }[] = [
  { tool: 'pen', icon: 'pen', label: 'Pen', key: 'P' },
  { tool: 'highlighter', icon: 'highlighter', label: 'Highlighter', key: 'H' },
  { tool: 'laser', icon: 'laser', label: 'Laser pointer (leaves no ink)', key: 'L' },
  { tool: 'eraser', icon: 'eraser', label: 'Eraser', key: 'E' },
  { tool: 'line', icon: 'line', label: 'Line', key: 'I' },
  { tool: 'arrow', icon: 'arrow', label: 'Arrow', key: 'A' },
  { tool: 'rect', icon: 'rect', label: 'Rectangle', key: 'R' },
  { tool: 'ellipse', icon: 'ellipse', label: 'Ellipse', key: 'O' },
  { tool: 'text', icon: 'text', label: 'Text', key: 'T' },
];
const KEY_TO_TOOL: Record<string, Tool> = Object.fromEntries(TOOL_BUTTONS.map((b) => [b.key.toLowerCase(), b.tool]));
const BOARDS = ['none', 'white', 'black'] as const;
type Board = (typeof BOARDS)[number];

/** Does this key event belong to the hold-to-click-through key? */
function isClickThroughKey(e: KeyboardEvent, key: Settings['clickThroughKey']): boolean {
  switch (key) {
    case 'CapsLock':
      return e.key === 'CapsLock';
    case '`':
      return e.code === 'Backquote';
    case 'F8':
      return e.key === 'F8';
    case 'RCtrl':
      return e.code === 'ControlRight';
    default:
      return false;
  }
}

const OVERFLOWS = /(auto|scroll|overlay)/;

function isEditable(t: EventTarget | null): boolean {
  return t instanceof HTMLElement && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName));
}

/** Short, stable CSS path for an element, used to find an inner scroller again later. */
function cssPath(el: Element): string {
  const parts: string[] = [];
  for (let n: Element | null = el; n && n !== document.documentElement; n = n.parentElement) {
    if (n.id) {
      parts.unshift(`#${CSS.escape(n.id)}`);
      break;
    }
    const parent: Element | null = n.parentElement;
    const index = parent ? Array.prototype.indexOf.call(parent.children, n) + 1 : 1;
    parts.unshift(`${n.tagName.toLowerCase()}:nth-child(${index})`);
  }
  return parts.join('>');
}

function queryPath(path: string): Element | null {
  try {
    return document.querySelector(path);
  } catch {
    return null;
  }
}

const frames = (n: number) =>
  new Promise<void>((resolve) => {
    const step = () => (--n <= 0 ? resolve() : requestAnimationFrame(step));
    requestAnimationFrame(step);
  });

export interface BootOptions {
  /** Identifies the document the ink belongs to; read again when the address changes. */
  docKey: () => string;
}

export function boot(options: BootOptions): void {
  // Once per page: the toolbar button injects ScrollPen into tabs that don't have it yet,
  // which can race with Chrome running the content script itself.
  const g = globalThis as { __scrollpenBooted?: boolean };
  if (g.__scrollpenBooted) return;
  g.__scrollpenBooted = true;
  new Overlay(options).start();
}

class Overlay {
  private docKey: string;
  private settings: Settings = DEFAULT_SETTINGS;
  private items: Item[] = [];
  private undoStack: Op[] = [];
  private redoStack: Op[] = [];
  private lastSaved = 0;
  private saveTimer: ReturnType<typeof setTimeout> | undefined;
  private savePending = false;
  private settingsTimer: ReturnType<typeof setTimeout> | undefined;
  private urlTimer: ReturnType<typeof setInterval> | undefined;
  /** Set for ScrollPen's own pages (the PDF viewer), which receive broadcast messages. */
  private tabId: number | undefined;
  private destroyed = false;

  private host: HTMLElement | null = null;
  private root!: ShadowRoot;
  private docBox!: HTMLDivElement;
  private screenDiv!: HTMLDivElement;
  private boardDiv!: HTMLDivElement;
  private laserSvg!: SVGSVGElement;
  private laserPath!: SVGPathElement;
  private laserDot!: SVGCircleElement;
  private cursorSvg!: SVGSVGElement;
  private cursorCircle!: SVGCircleElement;
  private capture!: HTMLDivElement;
  private toolbar!: HTMLDivElement;
  private popover!: HTMLDivElement;
  private toastEl!: HTMLDivElement;
  private toastTimer: ReturnType<typeof setTimeout> | undefined;
  private buttons = new Map<string, HTMLButtonElement>();
  private swatch!: HTMLSpanElement;
  private hideButton!: HTMLButtonElement;
  private pinButton!: HTMLButtonElement;
  private clickThrough = false;

  private layers = new Map<string, Layer>();
  private elems = new Map<string, SVGGraphicsElement>();
  private readonly image: HTMLImageElement | null =
    document.contentType.startsWith('image/') && document.images.length === 1 ? document.images[0] : null;

  private toolbarOpen = false;
  private active = false;
  private board: Board = 'none';
  private inkHidden = false;
  private drag: Drag | null = null;
  private pointerId: number | null = null;
  private editor: Editor | null = null;
  private laser: { x: number; y: number; t: number }[] = [];
  private laserLoop = false;
  private layoutQueued = false;
  private missingTargets = false;
  private lastMutationCheck = 0;
  private observers: { disconnect(): void }[] = [];

  constructor(private readonly options: BootOptions) {
    this.docKey = options.docKey();
  }

  start(): void {
    chrome.runtime.onMessage.addListener(this.onMessage);
    chrome.storage.onChanged.addListener(this.onStorage);
    this.urlTimer = setInterval(this.checkUrl, 800);
    void this.init();
  }

  private async init(): Promise<void> {
    if (chrome.tabs?.getCurrent) this.tabId = (await chrome.tabs.getCurrent())?.id;
    this.settings = await loadSettings();
    await this.loadPage();
    // "Show the toolbar on every page": open it ready to use, without taking the mouse.
    if (this.settings.startActive && !this.toolbarOpen && !this.destroyed) {
      this.ensureUI();
      this.openToolbar();
      this.setActive(false);
    }
  }

  /** False once the extension was updated/removed (this script is orphaned) or replaced. */
  private alive(): boolean {
    if (this.destroyed) return false;
    if (!chrome.runtime?.id || this.host?.dataset.replaced) {
      this.destroy();
      return false;
    }
    return true;
  }

  private destroy(): void {
    if (this.destroyed) return;
    this.destroyed = true;
    clearInterval(this.urlTimer);
    clearTimeout(this.saveTimer);
    for (const o of this.observers) o.disconnect();
    window.removeEventListener('keydown', this.onKey, true);
    window.removeEventListener('keyup', this.onKeyUp, true);
    window.removeEventListener('blur', this.endClickThrough);
    window.removeEventListener('scroll', this.queueLayout, true);
    window.removeEventListener('resize', this.queueLayout);
    window.removeEventListener('pagehide', this.flushSave);
    try {
      chrome.runtime.onMessage.removeListener(this.onMessage);
      chrome.storage.onChanged.removeListener(this.onStorage);
    } catch {
      // Orphaned: the extension APIs are gone already.
    }
    if (this.host && !this.host.dataset.replaced) this.host.remove();
  }

  // ---- messages and storage --------------------------------------------------------

  private onMessage = (message: unknown, _sender: chrome.runtime.MessageSender, sendResponse: (r: unknown) => void) => {
    if (!this.alive()) return;
    const m = message as { type?: string; tabId?: number; name?: string } | null;
    if (m?.type !== 'toggle' && m?.type !== 'command') return;
    // Broadcasts to ScrollPen's own pages name the tab they're for.
    if (this.tabId !== undefined && m.tabId !== this.tabId) return;
    if (m.type === 'toggle') this.toggle();
    else this.runCommand(m.name);
    sendResponse({ ok: true });
  };

  /** Keyboard shortcuts from chrome://extensions/shortcuts (they work with the toolbar closed too). */
  private runCommand(name: string | undefined): void {
    if (name === 'screenshot') {
      this.ensureUI();
      void this.screenshot();
    } else if (name === 'clear') {
      if (!this.host) return;
      this.clear();
    } else if (name === 'toggle-ink') {
      if (!this.host) return;
      this.setInkHidden(!this.inkHidden);
      this.toast(this.inkHidden ? 'Ink hidden.' : 'Ink shown.');
    } else if (name === 'toggle-mode') {
      this.ensureUI();
      this.setDocumentMode(!this.settings.documentMode);
    }
  }

  private onStorage = (changes: Record<string, chrome.storage.StorageChange>, area: string) => {
    if (area !== 'local' || !this.alive()) return;
    if (changes[SETTINGS_KEY]) {
      this.settings = sanitizeSettings(changes[SETTINGS_KEY].newValue);
      this.refresh();
    }
    const change = changes[inkKey(this.docKey)];
    if (!change) return;
    // The same page open in another tab saved, or saved ink was deleted in the options.
    const saved = sanitizeSaved(change.newValue, this.docKey);
    if (saved ? saved.updated === this.lastSaved : !this.items.length) return;
    if (this.drag) return;
    this.items = saved?.items ?? [];
    this.lastSaved = saved?.updated ?? 0;
    this.undoStack = [];
    this.redoStack = [];
    if (this.items.length) this.ensureUI();
    this.renderAll();
  };

  private async loadPage(): Promise<void> {
    const key = this.docKey;
    const saved = await loadInk(key);
    if (key !== this.docKey || this.destroyed) return;
    this.items = saved?.items ?? [];
    this.lastSaved = saved?.updated ?? 0;
    this.undoStack = [];
    this.redoStack = [];
    if (this.items.length) this.ensureUI();
    if (this.host) this.renderAll();
  }

  /** Single-page apps change the address without reloading: switch to that page's ink. */
  private checkUrl = () => {
    if (!this.alive()) return;
    const key = this.options.docKey();
    if (key === this.docKey) return;
    this.commitText();
    this.flushSave();
    this.drag = null;
    this.docKey = key;
    this.items = [];
    for (const [k, layer] of this.layers) {
      if (layer.kind === 'el') {
        layer.svg.remove();
        this.layers.delete(k);
      }
    }
    if (this.host) this.renderAll();
    void this.loadPage();
  };

  private scheduleSave(): void {
    this.savePending = true;
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(this.flushSave, SAVE_DELAY_MS);
  }

  private flushSave = () => {
    clearTimeout(this.saveTimer);
    if (!this.savePending || !chrome.runtime?.id) return;
    this.savePending = false;
    const updated = Date.now();
    this.lastSaved = updated;
    void saveInk(this.docKey, document.title, this.items, updated).catch(() => this.toast('Could not save the ink on this page.'));
  };

  private persistSettings(): void {
    clearTimeout(this.settingsTimer);
    this.settingsTimer = setTimeout(() => void saveSettings(this.settings).catch(() => undefined), SAVE_DELAY_MS);
  }

  // ---- DOM -------------------------------------------------------------------------

  private ensureUI(): void {
    if (this.host) return;
    // A previous copy of this script (before an extension update) may still be here.
    for (const old of document.querySelectorAll(HOST_TAG)) {
      (old as HTMLElement).dataset.replaced = '1';
      old.remove();
    }
    const host = document.createElement(HOST_TAG);
    host.setAttribute('style', HOST_STYLE);
    this.host = host;
    this.root = host.attachShadow({ mode: 'closed' });

    const style = document.createElement('style');
    style.textContent = SHADOW_CSS;
    this.docBox = this.div('doc');
    this.screenDiv = this.div('screen');
    this.boardDiv = this.div('board');
    const fixed = this.div('fixed');
    this.laserSvg = svg('svg', { class: 'laser' });
    this.laserPath = svg('path', { 'stroke-width': 5 });
    this.laserDot = svg('circle', { r: 6, cx: -100, cy: -100 });
    this.laserSvg.append(this.laserPath, this.laserDot);
    this.cursorSvg = svg('svg', { class: 'cursor' });
    this.cursorCircle = svg('circle', { r: ERASER_RADIUS, cx: -100, cy: -100 });
    this.cursorSvg.append(this.cursorCircle);
    fixed.append(this.laserSvg, this.cursorSvg);

    this.capture = this.div('capture');
    this.capture.tabIndex = -1;
    this.capture.addEventListener('pointerdown', this.onDown);
    this.capture.addEventListener('pointermove', this.onMove);
    this.capture.addEventListener('pointerup', this.onUp);
    this.capture.addEventListener('pointercancel', this.onUp);
    this.capture.addEventListener('pointerleave', () => this.moveCursor(null));
    this.capture.addEventListener('wheel', this.onWheel, { passive: false });
    this.capture.addEventListener('contextmenu', (e) => e.preventDefault());

    this.toolbar = this.buildToolbar();
    this.popover = this.div('popover');
    this.toastEl = this.div('toast');
    this.toastEl.setAttribute('role', 'status');
    this.root.append(
      style,
      this.docBox,
      this.screenDiv,
      this.boardDiv,
      fixed,
      this.capture,
      this.toolbar,
      this.popover,
      this.toastEl,
    );
    document.documentElement.append(host);

    window.addEventListener('keydown', this.onKey, true);
    window.addEventListener('keyup', this.onKeyUp, true);
    window.addEventListener('blur', this.endClickThrough);
    window.addEventListener('scroll', this.queueLayout, { capture: true, passive: true });
    window.addEventListener('resize', this.queueLayout, { passive: true });
    window.addEventListener('pagehide', this.flushSave);
    // Some pages rebuild <html>'s children; put ScrollPen back if it is removed.
    const keep = new MutationObserver(() => {
      if (!host.isConnected && !host.dataset.replaced && this.alive()) document.documentElement.append(host);
    });
    keep.observe(document.documentElement, { childList: true });
    const resize = new ResizeObserver(this.queueLayout);
    resize.observe(document.documentElement);
    if (this.image) resize.observe(this.image);
    // Inner scrollers saved earlier may appear only after the page renders them.
    const later = new MutationObserver(() => {
      const now = performance.now();
      if (!this.missingTargets || now - this.lastMutationCheck < 300) return;
      this.lastMutationCheck = now;
      this.queueLayout();
    });
    later.observe(document.documentElement, { childList: true, subtree: true });
    this.observers.push(keep, resize, later);
    this.ensureLayer('doc');
    this.layout();
  }

  private div(className: string): HTMLDivElement {
    const d = document.createElement('div');
    d.className = className;
    return d;
  }

  private ensureLayer(key: string): Layer {
    const existing = this.layers.get(key);
    if (existing) return existing;
    const kind: LayerKind =
      key === 'doc' || key === 'board' || key === 'image' || key === 'screen' ? key : 'el';
    const s = svg('svg', kind === 'el' ? { class: 'el' } : { width: 1, height: 1 });
    const g = svg('g');
    s.append(g);
    (kind === 'board' ? this.boardDiv : kind === 'screen' ? this.screenDiv : this.docBox).append(s);
    const target = kind === 'el' ? queryPath(key.slice(3)) : kind === 'image' ? this.image : null;
    const layer: Layer = { key, kind, svg: s, g, target, scale: 1 };
    this.layers.set(key, layer);
    this.layout();
    return layer;
  }

  private queueLayout = () => {
    if (this.layoutQueued) return;
    this.layoutQueued = true;
    requestAnimationFrame(() => {
      this.layoutQueued = false;
      if (this.alive()) this.layout();
    });
  };

  /** Places each layer over what it's attached to. The document layer never moves on scroll. */
  private layout(): void {
    if (!this.host) return;
    const hr = this.host.getBoundingClientRect();
    let missing = false;
    for (const layer of this.layers.values()) {
      if (layer.kind === 'doc') {
        layer.g.setAttribute('transform', `translate(${-(hr.left + window.scrollX)} ${-(hr.top + window.scrollY)})`);
      } else if (layer.kind === 'el') {
        if (!layer.target?.isConnected) layer.target = queryPath(layer.key.slice(3));
        const t = layer.target;
        if (!t) {
          layer.svg.style.display = 'none';
          missing = true;
          continue;
        }
        const r = t.getBoundingClientRect();
        Object.assign(layer.svg.style, {
          display: '',
          left: `${r.left + t.clientLeft - hr.left}px`,
          top: `${r.top + t.clientTop - hr.top}px`,
          width: `${t.clientWidth}px`,
          height: `${t.clientHeight}px`,
        });
        layer.g.setAttribute('transform', `translate(${-t.scrollLeft} ${-t.scrollTop})`);
      } else if (layer.kind === 'image' && this.image) {
        const r = this.image.getBoundingClientRect();
        layer.scale = this.image.naturalWidth > 0 ? r.width / this.image.naturalWidth : 1;
        Object.assign(layer.svg.style, { left: `${r.left - hr.left}px`, top: `${r.top - hr.top}px` });
        layer.g.setAttribute('transform', `scale(${layer.scale})`);
      }
    }
    this.missingTargets = missing;
  }

  private renderAll(): void {
    const wanted = new Set(this.items.map((i) => i.id));
    for (const [id, el] of this.elems) {
      if (!wanted.has(id)) {
        el.remove();
        this.elems.delete(id);
      }
    }
    for (const item of this.items) {
      if (this.elems.has(item.id)) continue;
      const el = renderItem(item);
      this.ensureLayer(item.layer).g.append(el);
      this.elems.set(item.id, el);
    }
    this.refresh();
  }

  // ---- modes -----------------------------------------------------------------------

  private toggle(): void {
    this.ensureUI();
    if (this.toolbarOpen) this.closeToolbar();
    else this.openToolbar();
  }

  private openToolbar(): void {
    this.toolbarOpen = true;
    this.toolbar.classList.add('open');
    this.placeToolbar();
    this.setActive(true);
  }

  private closeToolbar(): void {
    this.commitText();
    this.toolbarOpen = false;
    this.toolbar.classList.remove('open');
    this.closePopover();
    this.setActive(false);
    if (this.board !== 'none') this.setBoard('none');
  }

  private setDocumentMode(on: boolean): void {
    this.settings = { ...this.settings, documentMode: on };
    this.persistSettings();
    this.refresh();
    this.toast(on ? 'New ink follows the page as it scrolls.' : 'New ink stays put on the screen.');
  }

  private setMinimized(on: boolean): void {
    this.settings = { ...this.settings, toolbarMinimized: on };
    this.persistSettings();
    this.closePopover();
    this.refresh();
    this.placeToolbar();
  }

  private setActive(on: boolean): void {
    if (!on) this.commitText();
    this.active = on && this.toolbarOpen;
    if (!this.active) {
      this.drag = null;
      this.moveCursor(null);
    }
    this.refresh();
    if (this.active) this.capture.focus({ preventScroll: true });
  }

  private setTool(tool: Tool): void {
    this.commitText();
    this.settings = { ...this.settings, tool };
    this.persistSettings();
    this.setActive(true);
  }

  private setBoard(board: Board): void {
    this.commitText();
    this.board = board;
    this.boardDiv.className = `board ${board === 'none' ? '' : board}`;
    if (board !== 'none') this.setActive(true);
    this.refresh();
    if (this.toolbarOpen)
      this.toast(
        board === 'white'
          ? 'Whiteboard. Press K for a blackboard, W to go back to the page.'
          : board === 'black'
            ? 'Blackboard. Press K to go back to the page.'
            : 'Back to the page.',
      );
  }

  private setInkHidden(hidden: boolean): void {
    this.inkHidden = hidden;
    this.docBox.style.display = hidden ? 'none' : '';
    this.screenDiv.style.display = hidden ? 'none' : '';
    for (const layer of this.layers.values()) if (layer.kind === 'board') layer.svg.style.display = hidden ? 'none' : '';
    this.refresh();
  }

  private refresh(): void {
    if (!this.host) return;
    const tool = this.settings.tool;
    this.capture.className = `capture${this.active ? ' on' : ''}${this.clickThrough ? ' through' : ''}${tool === 'eraser' ? ' eraser' : tool === 'text' ? ' text' : tool === 'laser' ? ' laser' : ''}`;
    this.toolbar.classList.toggle('mini', this.settings.toolbarMinimized);
    this.pinButton.setAttribute('aria-pressed', String(this.settings.documentMode));
    this.pinButton.replaceChildren(icon(this.settings.documentMode ? 'pin' : 'pinOff'));
    this.pinButton.title = this.settings.documentMode
      ? 'Ink follows the page as it scrolls (click to keep new ink fixed on the screen)'
      : 'New ink stays fixed on the screen (click to make it follow the page)';
    for (const b of TOOL_BUTTONS) this.buttons.get(b.tool)?.setAttribute('aria-pressed', String(this.active && tool === b.tool));
    this.buttons.get('cursor')?.setAttribute('aria-pressed', String(!this.active));
    this.swatch.style.background = this.settings.color;
    this.buttons.get('undo')!.disabled = !this.undoStack.length;
    this.buttons.get('redo')!.disabled = !this.redoStack.length;
    this.buttons.get('clear')!.disabled = !this.visibleItems().length;
    this.buttons.get('board')?.setAttribute('aria-pressed', String(this.board !== 'none'));
    this.hideButton.setAttribute('aria-pressed', String(this.inkHidden));
    this.hideButton.replaceChildren(icon(this.inkHidden ? 'eyeOff' : 'eye'));
    this.hideButton.title = this.inkHidden ? 'Show ink' : 'Hide ink';
  }

  /** Items on what's showing: the board's while a board is open, else the page's. */
  private visibleItems(): Item[] {
    return this.items.filter((i) => (this.board === 'none' ? i.layer !== 'board' : i.layer === 'board'));
  }

  // ---- toolbar ---------------------------------------------------------------------

  private button(name: string, iconName: IconName, title: string, onClick: () => void): HTMLButtonElement {
    const b = document.createElement('button');
    b.type = 'button';
    b.title = title;
    b.setAttribute('aria-label', title);
    b.append(icon(iconName));
    b.addEventListener('click', () => {
      if (this.alive()) onClick();
    });
    this.buttons.set(name, b);
    return b;
  }

  private buildToolbar(): HTMLDivElement {
    const tb = this.div('toolbar');
    tb.setAttribute('role', 'toolbar');
    tb.setAttribute('aria-label', 'ScrollPen');
    tb.setAttribute('aria-orientation', 'vertical');
    const sep = () => this.div('sep');

    const grip = this.button('grip', 'grip', 'Drag to move', () => undefined);
    grip.classList.add('grip');
    grip.addEventListener('pointerdown', this.onGripDown);

    tb.append(grip, this.button('cursor', 'cursor', 'Use the page normally (Esc)', () => this.setActive(false)), sep());
    for (const t of TOOL_BUTTONS) tb.append(this.button(t.tool, t.icon, `${t.label} (${t.key})`, () => this.setTool(t.tool)));

    const restore = this.button('restore', 'restore', 'Show the whole toolbar', () => this.setMinimized(false));
    restore.classList.add('restore');
    tb.append(restore);

    const color = this.button('color', 'pen', 'Colour (1-9) and size ([ and ])', () => this.togglePopover());
    this.swatch = document.createElement('span');
    this.swatch.className = 'swatch';
    color.replaceChildren(this.swatch);
    tb.append(sep(), color, sep());

    tb.append(
      this.button('undo', 'undo', 'Undo (Ctrl+Z)', () => this.undo()),
      this.button('redo', 'redo', 'Redo (Ctrl+Y)', () => this.redo()),
      this.button('clear', 'clear', 'Erase everything on this page (Delete)', () => this.clear()),
      sep(),
    );
    const board = this.button('board', 'board', 'Whiteboard (W) / Blackboard (K)', () => this.cycleBoard());
    this.hideButton = this.button('hide', 'eye', 'Hide ink', () => this.setInkHidden(!this.inkHidden));
    this.pinButton = this.button('pin', 'pin', 'Follow scrolling', () => this.setDocumentMode(!this.settings.documentMode));
    tb.append(
      board,
      this.pinButton,
      this.hideButton,
      this.button('camera', 'camera', 'Save a screenshot of this page', () => void this.screenshot()),
      this.button('settings', 'settings', 'ScrollPen settings and saved pages', () =>
        void chrome.runtime.sendMessage({ type: 'options' }).catch(() => undefined),
      ),
      sep(),
      this.button('minimize', 'minimize', 'Make the toolbar small', () => this.setMinimized(true)),
      this.button('close', 'close', 'Close ScrollPen (Alt+Shift+D). Your ink stays.', () => this.closeToolbar()),
    );
    return tb;
  }

  private placeToolbar(x?: number, y?: number): void {
    const vw = document.documentElement.clientWidth || window.innerWidth;
    const vh = window.innerHeight;
    const h = this.toolbar.offsetHeight || 600;
    const pos = x !== undefined && y !== undefined ? { x, y } : this.settings.toolbar;
    const left = pos ? pos.x : vw - TOOLBAR_WIDTH - 16;
    const top = pos ? pos.y : 72;
    this.toolbar.style.left = `${Math.max(4, Math.min(left, vw - TOOLBAR_WIDTH - 4))}px`;
    this.toolbar.style.top = `${Math.max(4, Math.min(top, vh - Math.min(h, vh - 16) - 4))}px`;
  }

  private onGripDown = (e: PointerEvent) => {
    if (e.button !== 0) return;
    e.preventDefault();
    const grip = e.currentTarget as HTMLElement;
    grip.setPointerCapture(e.pointerId);
    const r = this.toolbar.getBoundingClientRect();
    const dx = e.clientX - r.left, dy = e.clientY - r.top;
    const move = (ev: PointerEvent) => this.placeToolbar(ev.clientX - dx, ev.clientY - dy);
    const up = () => {
      grip.removeEventListener('pointermove', move);
      grip.removeEventListener('pointerup', up);
      grip.removeEventListener('pointercancel', up);
      const nr = this.toolbar.getBoundingClientRect();
      this.settings = { ...this.settings, toolbar: { x: Math.round(nr.left), y: Math.round(nr.top) } };
      this.persistSettings();
    };
    grip.addEventListener('pointermove', move);
    grip.addEventListener('pointerup', up);
    grip.addEventListener('pointercancel', up);
  };

  private togglePopover(): void {
    if (this.popover.classList.contains('open')) {
      this.closePopover();
      return;
    }
    const p = this.popover;
    p.replaceChildren();
    const swatches = this.div('swatches');
    for (const c of this.settings.palette) {
      const b = document.createElement('button');
      b.type = 'button';
      b.title = c;
      b.setAttribute('aria-label', `Color ${c}`);
      b.setAttribute('aria-pressed', String(c.toLowerCase() === this.settings.color.toLowerCase()));
      b.style.background = c;
      b.addEventListener('click', () => {
        this.settings = { ...this.settings, color: c };
        this.persistSettings();
        if (this.settings.tool === 'eraser' || this.settings.tool === 'laser') this.settings = { ...this.settings, tool: 'pen' };
        this.closePopover();
        this.setActive(true);
      });
      swatches.append(b);
    }
    const key = sizeKey(this.settings.tool);
    const [min, max] = SIZE_RANGE[key];
    const label = document.createElement('label');
    const name = document.createElement('span');
    name.textContent = key === 'text' ? 'Text size' : key === 'highlighter' ? 'Highlighter size' : key === 'shape' ? 'Shape line size' : 'Pen size';
    const value = document.createElement('span');
    value.textContent = String(this.settings.sizes[key]);
    label.append(name, value);
    const range = document.createElement('input');
    range.type = 'range';
    range.min = String(min);
    range.max = String(max);
    range.value = String(this.settings.sizes[key]);
    range.setAttribute('aria-label', name.textContent);
    range.addEventListener('input', () => {
      const v = Number(range.value);
      this.settings = { ...this.settings, sizes: { ...this.settings.sizes, [key]: v } };
      value.textContent = String(v);
      this.persistSettings();
    });
    p.append(swatches, label, range);
    p.classList.add('open');
    const tb = this.toolbar.getBoundingClientRect();
    const btn = this.buttons.get('color')!.getBoundingClientRect();
    const w = p.offsetWidth, h = p.offsetHeight;
    const left = tb.left - w - 8 >= 4 ? tb.left - w - 8 : tb.right + 8;
    p.style.left = `${left}px`;
    p.style.top = `${Math.max(4, Math.min(btn.top - 8, window.innerHeight - h - 4))}px`;
  }

  private closePopover(): void {
    this.popover?.classList.remove('open');
  }

  private cycleBoard(): void {
    this.setBoard(BOARDS[(BOARDS.indexOf(this.board) + 1) % BOARDS.length]);
  }

  private setColorIndex(i: number): void {
    const c = this.settings.palette[i];
    if (!c) return;
    this.settings = { ...this.settings, color: c };
    this.persistSettings();
    this.refresh();
  }

  private stepSize(up: boolean): void {
    const key = sizeKey(this.settings.tool);
    const [min, max] = SIZE_RANGE[key];
    const cur = this.settings.sizes[key];
    const next = Math.round(Math.min(max, Math.max(min, up ? cur * SIZE_STEP : cur / SIZE_STEP)));
    const v = next === cur ? Math.min(max, Math.max(min, cur + (up ? 1 : -1))) : next;
    this.settings = { ...this.settings, sizes: { ...this.settings.sizes, [key]: v } };
    this.persistSettings();
    this.toast(`Size ${v}`);
  }

  /** Ctrl+Up/Down: move this page's ink up or down (to line it up again after a page changed). */
  private nudge(dy: number): void {
    const moved = this.items.filter((i) => i.layer === 'doc' || i.layer.startsWith('el:'));
    if (!moved.length) return;
    const copies = moved.map((i) => ({
      ...i,
      id: newId(),
      points: i.points.map((v, n) => (i.kind === 'pen' || i.kind === 'highlighter' ? n % 3 === 1 : n % 2 === 1) ? v + dy : v),
    }));
    this.apply({ add: copies, remove: moved });
  }

  private startClickThrough(): void {
    if (this.clickThrough || !this.active) return;
    this.commitText();
    this.clickThrough = true;
    this.refresh();
  }

  private endClickThrough = () => {
    if (!this.clickThrough) return;
    this.clickThrough = false;
    if (this.host) this.refresh();
  };

  private onKeyUp = (e: KeyboardEvent) => {
    if (this.clickThrough && isClickThroughKey(e, this.settings.clickThroughKey)) this.endClickThrough();
  };

  // ---- editing ---------------------------------------------------------------------

  private apply(op: Op, record = true): void {
    const removeIds = new Set(op.remove.map((i) => i.id));
    this.items = this.items.filter((i) => !removeIds.has(i.id)).concat(op.add);
    if (record) this.record(op);
    this.renderAll();
    this.scheduleSave();
  }

  private record(op: Op): void {
    this.undoStack.push(op);
    if (this.undoStack.length > MAX_UNDO) this.undoStack.shift();
    this.redoStack = [];
  }

  private undo(): void {
    this.commitText();
    const op = this.undoStack.pop();
    if (!op) return;
    this.apply({ add: op.remove, remove: op.add }, false);
    this.redoStack.push(op);
    this.refresh();
  }

  private redo(): void {
    const op = this.redoStack.pop();
    if (!op) return;
    this.apply(op, false);
    this.undoStack.push(op);
    this.refresh();
  }

  private clear(): void {
    this.commitText();
    const items = this.visibleItems();
    if (!items.length) return;
    this.apply({ add: [], remove: items });
    this.toast('Erased. Press Ctrl+Z to bring it back.');
  }

  // ---- pointer input ---------------------------------------------------------------

  /** The layer a stroke starting at this point belongs to. */
  private layerAt(cx: number, cy: number): Layer {
    if (this.board !== 'none') return this.ensureLayer('board');
    if (!this.settings.documentMode) return this.ensureLayer('screen');
    if (this.image) return this.ensureLayer('image');
    const scroller = this.scrollerAt(cx, cy, 0, 0);
    if (!scroller) return this.ensureLayer('doc');
    const layer = this.ensureLayer(`el:${cssPath(scroller)}`);
    layer.target = scroller;
    this.layout();
    return layer;
  }

  /**
   * The inner element under the point that scrolls (in the given direction, if any), or
   * null when that is the page itself.
   */
  private scrollerAt(cx: number, cy: number, dx: number, dy: number): Element | null {
    for (const start of document.elementsFromPoint(cx, cy)) {
      if (start === this.host) continue;
      for (let el: Element | null = start; el; el = el.parentElement) {
        if (el === document.documentElement || el === document.scrollingElement) return null;
        const cs = getComputedStyle(el);
        const canY = OVERFLOWS.test(cs.overflowY) && el.scrollHeight > el.clientHeight + 1;
        const canX = OVERFLOWS.test(cs.overflowX) && el.scrollWidth > el.clientWidth + 1;
        if (!dx && !dy) {
          if (canY || canX) return el;
          continue;
        }
        const roomY = dy > 0 ? el.scrollTop + el.clientHeight < el.scrollHeight - 1 : el.scrollTop > 0;
        const roomX = dx > 0 ? el.scrollLeft + el.clientWidth < el.scrollWidth - 1 : el.scrollLeft > 0;
        if ((dy && canY && roomY) || (dx && canX && roomX)) return el;
      }
      return null;
    }
    return null;
  }

  private toLayer(layer: Layer, cx: number, cy: number): [number, number] {
    const round = (n: number) => Math.round(n * 10) / 10;
    if (layer.kind === 'board' || layer.kind === 'screen') return [round(cx), round(cy)];
    if (layer.kind === 'doc') return [round(cx + window.scrollX), round(cy + window.scrollY)];
    const t = layer.target;
    if (!t) return [round(cx), round(cy)];
    const r = t.getBoundingClientRect();
    if (layer.kind === 'image') return [round((cx - r.left) / layer.scale), round((cy - r.top) / layer.scale)];
    return [round(cx - r.left - t.clientLeft + t.scrollLeft), round(cy - r.top - t.clientTop + t.scrollTop)];
  }

  private onDown = (e: PointerEvent) => {
    if (!this.alive() || !this.active) return;
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    e.preventDefault();
    this.closePopover();
    if (this.editor) {
      this.commitText();
      if (this.settings.tool !== 'text') return;
    }
    this.capture.focus({ preventScroll: true });
    // A pen's eraser end (or barrel eraser button) erases whatever the tool.
    const tool: Tool = e.pointerType === 'pen' && e.buttons & 32 ? 'eraser' : this.settings.tool;
    this.capture.setPointerCapture(e.pointerId);
    if (tool === 'text') {
      // Opened on release: the press itself moves keyboard focus, which would close it.
      this.pointerId = e.pointerId;
      this.drag = { tool: 'place-text', x: e.clientX, y: e.clientY };
      return;
    }
    this.pointerId = e.pointerId;
    if (tool === 'laser') {
      this.drag = { tool };
      this.addLaser(e.clientX, e.clientY);
      return;
    }
    if (tool === 'eraser') {
      this.drag = { tool, removed: [] };
      this.eraseAt(e.clientX, e.clientY);
      this.moveCursor(e.clientX, e.clientY, 'eraser');
      return;
    }
    const layer = this.layerAt(e.clientX, e.clientY);
    const [x, y] = this.toLayer(layer, e.clientX, e.clientY);
    const kind = tool as Kind;
    const freehand = kind === 'pen' || kind === 'highlighter';
    const item: Item = {
      id: newId(),
      kind,
      layer: layer.key,
      color: this.settings.color,
      width: Math.round((this.settings.sizes[sizeKey(tool)] / layer.scale) * 100) / 100,
      points: freehand ? [x, y, pressure(e)] : [x, y, x, y],
    };
    if ((kind === 'rect' || kind === 'ellipse') && this.settings.fill) item.fill = true;
    const el = renderItem(item, true);
    layer.g.append(el);
    this.drag = { tool: kind, item, layer, el, sx: e.clientX, sy: e.clientY };
  };

  private onMove = (e: PointerEvent) => {
    if (!this.alive()) return;
    const events = e.getCoalescedEvents?.().length ? e.getCoalescedEvents() : [e];
    const drag = this.drag;
    if (!drag || e.pointerId !== this.pointerId) {
      if (this.active) this.moveCursor(e.clientX, e.clientY);
      return;
    }
    if (drag.tool === 'place-text') return;
    if (drag.tool === 'laser') {
      for (const ev of events) this.addLaser(ev.clientX, ev.clientY);
      return;
    }
    if (drag.tool === 'eraser') {
      for (const ev of events) this.eraseAt(ev.clientX, ev.clientY);
      this.moveCursor(e.clientX, e.clientY, 'eraser');
      return;
    }
    const { item, layer } = drag;
    if (item.kind === 'pen' || item.kind === 'highlighter') {
      const p = item.points;
      for (const ev of events) {
        const [x, y] = this.toLayer(layer, ev.clientX, ev.clientY);
        if (Math.hypot(x - p[p.length - 3], y - p[p.length - 2]) < 0.6 / layer.scale) continue;
        p.push(x, y, pressure(ev));
      }
    } else {
      let [x, y] = this.toLayer(layer, e.clientX, e.clientY);
      if (e.shiftKey) [x, y] = constrain(item.kind, item.points[0], item.points[1], x, y);
      item.points[2] = x;
      item.points[3] = y;
    }
    const el = renderItem(item, true);
    drag.el.replaceWith(el);
    drag.el = el;
  };

  private onUp = (e: PointerEvent) => {
    const drag = this.drag;
    if (!drag || e.pointerId !== this.pointerId) return;
    this.drag = null;
    this.pointerId = null;
    if (this.capture.hasPointerCapture(e.pointerId)) this.capture.releasePointerCapture(e.pointerId);
    if (!this.alive()) return;
    if (drag.tool === 'laser') return;
    if (drag.tool === 'place-text') {
      this.openText(drag.x, drag.y);
      return;
    }
    if (drag.tool === 'eraser') {
      if (drag.removed.length) this.record({ add: [], remove: drag.removed });
      this.refresh();
      if (e.pointerType !== 'mouse') this.moveCursor(null);
      return;
    }
    drag.el.remove();
    // A click with a shape tool draws nothing.
    if (drag.tool !== 'pen' && drag.tool !== 'highlighter' && Math.hypot(e.clientX - drag.sx, e.clientY - drag.sy) < 3) return;
    this.apply({ add: [drag.item], remove: [] });
  };

  private onWheel = (e: WheelEvent) => {
    if (e.ctrlKey || !this.alive()) return; // Ctrl+wheel is the browser's zoom
    this.commitText();
    // The page scrolls by itself under the drawing layer; inner panels need a hand.
    const scroller = this.scrollerAt(e.clientX, e.clientY, e.deltaX, e.deltaY);
    if (!scroller) return;
    e.preventDefault();
    const k = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? scroller.clientHeight : 1;
    scroller.scrollBy({ left: e.deltaX * k, top: e.deltaY * k, behavior: 'instant' });
  };

  private onKey = (e: KeyboardEvent) => {
    if (!this.toolbarOpen || !this.alive()) return;
    // Typing in ScrollPen's text box. (composedPath() can't see into the closed shadow root.)
    if (this.editor && this.root.activeElement === this.editor.ta) return;
    if (e.key === 'Escape' && this.popover.classList.contains('open')) {
      this.closePopover();
      e.preventDefault();
      return;
    }
    if (!this.active || e.isComposing) return;
    // Hold to click through: the page gets the mouse until the key is let go.
    if (isClickThroughKey(e, this.settings.clickThroughKey)) {
      this.startClickThrough();
      if (e.key !== 'CapsLock' && e.key !== 'Control') e.preventDefault();
      return;
    }
    if (this.clickThrough || (e.target !== this.host && isEditable(e.target))) return;
    const k = e.key.toLowerCase();
    const mod = e.ctrlKey || e.metaKey;
    let handled = true;
    if (mod && !e.altKey && k === 'z') {
      if (e.shiftKey) this.redo();
      else this.undo();
    } else if (mod && !e.altKey && k === 'y') this.redo();
    else if (mod && !e.altKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown'))
      this.nudge((e.key === 'ArrowUp' ? -1 : 1) * (e.shiftKey ? 1 : 10));
    else if (mod || e.altKey) handled = false;
    else if (e.key === '[' || e.key === ']') this.stepSize(e.key === ']');
    else if (e.shiftKey) handled = false;
    else if (k === 'escape') this.setActive(false);
    else if (k === 'delete') this.clear();
    else if (k === 'w') this.setBoard(this.board === 'white' ? 'none' : 'white');
    else if (k === 'k') this.setBoard(this.board === 'black' ? 'none' : 'black');
    else if (/^[1-9]$/.test(k)) this.setColorIndex(Number(k) - 1);
    else if (KEY_TO_TOOL[k]) this.setTool(KEY_TO_TOOL[k]);
    else handled = false;
    if (handled) {
      e.preventDefault();
      e.stopImmediatePropagation();
    }
  };

  private eraseAt(cx: number, cy: number): void {
    const drag = this.drag;
    if (!drag || drag.tool !== 'eraser' || this.inkHidden) return;
    const hit: Item[] = [];
    for (const item of this.visibleItems()) {
      const layer = this.layers.get(item.layer);
      if (!layer || layer.svg.style.display === 'none') continue;
      const [x, y] = this.toLayer(layer, cx, cy);
      const el = this.elems.get(item.id);
      const box = item.kind === 'text' && el ? el.getBBox() : null;
      if (hitItem(item, x, y, ERASER_RADIUS / layer.scale, box)) hit.push(item);
    }
    if (!hit.length) return;
    const ids = new Set(hit.map((i) => i.id));
    this.items = this.items.filter((i) => !ids.has(i.id));
    drag.removed.push(...hit);
    this.renderAll();
    this.scheduleSave();
  }

  private moveCursor(cx: number | null, cy = 0, forced?: Tool): void {
    if (!this.host) return;
    const tool = forced ?? this.settings.tool;
    const show = cx !== null && this.active && (tool === 'eraser' || tool === 'laser');
    if (!show) {
      this.cursorCircle.setAttribute('cx', '-100');
      if (!this.drag || this.drag.tool !== 'laser') this.laserDot.setAttribute('cx', '-100');
      return;
    }
    if (tool === 'eraser') {
      this.cursorCircle.setAttribute('cx', String(cx));
      this.cursorCircle.setAttribute('cy', String(cy));
      this.laserDot.setAttribute('cx', '-100');
    } else {
      this.laserDot.setAttribute('cx', String(cx));
      this.laserDot.setAttribute('cy', String(cy));
      this.cursorCircle.setAttribute('cx', '-100');
    }
  }

  private addLaser(x: number, y: number): void {
    this.laser.push({ x, y, t: performance.now() });
    this.laserDot.setAttribute('cx', String(x));
    this.laserDot.setAttribute('cy', String(y));
    if (this.laserLoop) return;
    this.laserLoop = true;
    const tick = () => {
      const now = performance.now();
      const fade = this.settings.laserFadeSeconds * 1000;
      this.laser = this.laser.filter((p) => now - p.t < fade);
      const d = this.laser.map((p, i) => `${i ? 'L' : 'M'} ${p.x} ${p.y}`).join(' ');
      this.laserPath.setAttribute('d', d);
      if (this.laser.length || this.drag?.tool === 'laser') requestAnimationFrame(tick);
      else this.laserLoop = false;
    };
    requestAnimationFrame(tick);
  }

  // ---- text ------------------------------------------------------------------------

  private openText(cx: number, cy: number): void {
    const layer = this.layerAt(cx, cy);
    const size = this.settings.sizes.text;
    const ta = document.createElement('textarea');
    ta.className = 'text-edit';
    ta.rows = 1;
    ta.spellcheck = false;
    ta.setAttribute('aria-label', 'Text to write on the page');
    Object.assign(ta.style, { left: `${cx}px`, top: `${cy}px`, fontSize: `${size}px`, color: this.settings.color });
    // Keep typing inside ScrollPen: the page's own key handlers shouldn't react to it.
    for (const type of ['keydown', 'keyup', 'keypress', 'beforeinput', 'input'])
      ta.addEventListener(type, (ev) => ev.stopPropagation());
    // Enter starts a new line; Ctrl+Enter or Esc finishes the text.
    ta.addEventListener('keydown', (ev) => {
      if ((ev.key === 'Enter' && (ev.ctrlKey || ev.metaKey)) || ev.key === 'Escape') {
        ev.preventDefault();
        this.commitText();
      }
    });
    // Clicking elsewhere finishes the text; a focus change that comes straight back doesn't.
    ta.addEventListener('blur', () =>
      requestAnimationFrame(() => {
        if (this.editor?.ta === ta && this.root.activeElement !== ta) this.commitText();
      }),
    );
    this.root.append(ta);
    this.editor = { ta, layer, at: this.toLayer(layer, cx, cy), color: this.settings.color, size };
    ta.focus({ preventScroll: true });
  }

  private commitText(): void {
    const ed = this.editor;
    if (!ed) return;
    this.editor = null;
    const text = ed.ta.value.replace(/\s+$/, '');
    ed.ta.remove();
    if (this.active) this.capture.focus({ preventScroll: true });
    if (!text) return;
    this.apply({
      add: [
        {
          id: newId(),
          kind: 'text',
          layer: ed.layer.key,
          color: ed.color,
          width: Math.round((ed.size / ed.layer.scale) * 100) / 100,
          points: ed.at,
          text: text.slice(0, 5000),
        },
      ],
      remove: [],
    });
  }

  // ---- screenshot and messages -----------------------------------------------------

  private async screenshot(): Promise<void> {
    this.commitText();
    this.closePopover();
    this.toolbar.style.visibility = 'hidden';
    this.moveCursor(null);
    await frames(2);
    try {
      const res = (await chrome.runtime.sendMessage({ type: 'capture' })) as { dataUrl?: string } | undefined;
      if (!res?.dataUrl?.startsWith('data:image/png;base64,')) throw new Error('no image');
      const bytes = Uint8Array.from(atob(res.dataUrl.slice(22)), (c) => c.charCodeAt(0));
      const url = URL.createObjectURL(new Blob([bytes], { type: 'image/png' }));
      const a = document.createElement('a');
      const d = new Date();
      const pad = (n: number) => String(n).padStart(2, '0');
      a.download = `ScrollPen ${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}-${pad(d.getMinutes())}-${pad(d.getSeconds())}.png`;
      a.href = url;
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 30_000);
      this.toolbar.style.visibility = '';
      this.toast('Screenshot saved to your Downloads folder.');
    } catch {
      this.toolbar.style.visibility = '';
      this.toast("Couldn't take a screenshot. Click the ScrollPen icon once, then try again.");
    }
  }

  private toast(message: string): void {
    if (!this.host) return;
    this.toastEl.textContent = message;
    this.toastEl.classList.add('show');
    clearTimeout(this.toastTimer);
    this.toastTimer = setTimeout(() => this.toastEl.classList.remove('show'), 2800);
  }
}

function pressure(e: PointerEvent): number {
  return e.pointerType === 'pen' && e.pressure > 0 ? Math.round(e.pressure * 100) / 100 : 0.5;
}

/** Shift while dragging: lines snap to 45 degrees, boxes become squares and circles. */
function constrain(kind: Kind, x1: number, y1: number, x2: number, y2: number): [number, number] {
  const dx = x2 - x1, dy = y2 - y1;
  if (kind === 'rect' || kind === 'ellipse') {
    const s = Math.max(Math.abs(dx), Math.abs(dy));
    return [x1 + Math.sign(dx || 1) * s, y1 + Math.sign(dy || 1) * s];
  }
  const a = Math.round(Math.atan2(dy, dx) / (Math.PI / 4)) * (Math.PI / 4);
  const len = Math.hypot(dx, dy);
  return [x1 + Math.cos(a) * len, y1 + Math.sin(a) * len];
}
