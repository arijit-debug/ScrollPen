/**
 * Messages the browser-sync extension sends to the ScrollPen desktop app
 * over ws://127.0.0.1:<port>. Nothing here ever leaves the machine.
 */

export const SYNC_PROTOCOL_VERSION = 1;
export const SYNC_DEFAULT_PORT = 47615;

/** Sent once per WebSocket connection. */
export interface HelloMessage {
  type: 'hello';
  v: typeof SYNC_PROTOCOL_VERSION;
  extVersion: string;
  userAgent: string;
}

/** Where the scrolled content sits, in CSS pixels relative to the viewport. */
export interface ScrollerRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

/** A scroll position change in a tab (document or an inner scrolling element). */
export interface ScrollMessage {
  type: 'scroll';
  v: typeof SYNC_PROTOCOL_VERSION;
  tabId: number;
  /** Browser window id (added by the service worker). */
  windowId?: number;
  /** Tab zoom factor (added by the service worker). */
  zoom?: number;
  /** False when the tab is in the background. */
  visible: boolean;
  /** Stable key for the document: origin + path + query (no hash). */
  docKey: string;
  /** 'document' for the page itself, otherwise a CSS-path for the inner scroller. */
  scroller: 'document' | string;
  scrollerRect: ScrollerRect;
  /**
   * Scroller 'image' (an image opened in a tab): CSS px per image px. Chrome centres the
   * picture and shrinks large ones to fit, so ink is anchored to the image itself.
   */
  contentScale?: number;
  scrollX: number;
  scrollY: number;
  scrollWidth: number;
  scrollHeight: number;
  viewportWidth: number;
  viewportHeight: number;
  devicePixelRatio: number;
  /** Viewport origin on screen in CSS px (window.screenX/Y + chrome offsets). */
  screenX: number;
  screenY: number;
  /**
   * document.title. Chrome titles its window after the active tab, so the app can tell
   * which tab a window shows even when several windows have the same size and place.
   */
  title?: string;
  ts: number;
}

/** Browser zoom changed for a tab (Ctrl+wheel, Ctrl+=, etc.). */
export interface ZoomMessage {
  type: 'zoom';
  v: typeof SYNC_PROTOCOL_VERSION;
  tabId: number;
  zoom: number;
}

/** A tab became the active one in its window. */
export interface FocusMessage {
  type: 'focus';
  v: typeof SYNC_PROTOCOL_VERSION;
  tabId: number;
  windowId: number;
}

/** The tab navigated to a new document or was closed. */
export interface NavigateMessage {
  type: 'navigate';
  v: typeof SYNC_PROTOCOL_VERSION;
  tabId: number;
  docKey: string | null;
}

export type SyncMessage =
  | HelloMessage
  | ScrollMessage
  | ZoomMessage
  | FocusMessage
  | NavigateMessage;

/**
 * App -> extension: the user started drawing at (x, y) in this tab (CSS px, relative to
 * the viewport). The page reports the scrolling element there, so ink follows inner
 * scrollers (chat panes, editors) even before they have scrolled.
 */
export interface ProbeMessage {
  type: 'probe';
  v: typeof SYNC_PROTOCOL_VERSION;
  tabId: number;
  x: number;
  y: number;
}

export type HostMessage = ProbeMessage;

/** Content script -> service worker (the worker adds tabId before forwarding). */
export type ContentReport =
  | Omit<ScrollMessage, 'tabId' | 'windowId' | 'zoom'>
  | { type: 'navigate'; v: typeof SYNC_PROTOCOL_VERSION; docKey: string };
