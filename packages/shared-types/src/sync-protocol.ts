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
  /** Stable key for the document: origin + path + query (no hash). */
  docKey: string;
  /** 'document' for the page itself, otherwise a CSS-path for the inner scroller. */
  scroller: 'document' | string;
  scrollerRect: ScrollerRect;
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

/** Content script -> service worker (the worker adds tabId before forwarding). */
export type ContentReport =
  | Omit<ScrollMessage, 'tabId'>
  | { type: 'navigate'; v: typeof SYNC_PROTOCOL_VERSION; docKey: string };
