import {
  NATIVE_HOST_NAME,
  SYNC_PROTOCOL_VERSION,
  type ContentReport,
  type HostMessage,
  type SyncMessage,
} from '@scrollpen/shared-types';

// The app is reached through Chrome native messaging: Chrome starts ScrollPen's helper,
// registered for this Windows user only, and the helper relays over a pipe only this user
// can open (see sync-protocol.ts). Nothing listens on a network port, so no other program
// or Windows user can pose as the app. Tests and dev tools set chrome.storage.local "port"
// to use a WebSocket on 127.0.0.1 instead (the mock host, or the app's dev server).
interface Link {
  send(message: object): void;
  close(): void;
}
let link: Link | null = null;
/** The app itself is on the other end (the native helper can be up while the app isn't). */
let ready = false;
let welcomed = false;
let connecting = false;
let retryMs = 500;
// After a failed attempt (the helper isn't installed, or the dev server isn't running) don't
// try again before this time. Page reports arriving meanwhile wait in the outbox.
let retryAt = 0;
let keepAlive: ReturnType<typeof setInterval> | undefined;
const outbox: SyncMessage[] = [];

// For tests and troubleshooting from the service worker's console.
(globalThis as { scrollpenLink?: () => object }).scrollpenLink = () => ({
  transport: link ? (link instanceof WebSocketLink ? 'websocket' : 'native') : 'none',
  ready,
  welcomed,
});

async function devPort(): Promise<number | undefined> {
  const { port } = await chrome.storage.local.get('port');
  return Number.isInteger(port) && port > 0 && port < 65536 ? (port as number) : undefined;
}

function linkUp(l: Link): void {
  if (link !== l) return;
  ready = true;
  retryMs = 500;
  retryAt = 0;
  l.send({
    type: 'hello',
    v: SYNC_PROTOCOL_VERSION,
    extVersion: chrome.runtime.getManifest().version,
    userAgent: navigator.userAgent,
  } satisfies SyncMessage);
  for (const m of outbox.splice(0)) l.send(m);
  void resync();
}

function linkLost(l: Link): void {
  if (link !== l) return;
  link = null;
  ready = welcomed = false;
  clearInterval(keepAlive);
  retryAt = Date.now() + retryMs;
  setTimeout(() => void connect(), retryMs);
  retryMs = Math.min(retryMs * 2, 10_000);
}

function onAppMessage(l: Link, message: HostMessage | null): void {
  if (link !== l || message?.v !== SYNC_PROTOCOL_VERSION) return;
  if (message.type === 'host' && l instanceof NativeLink) {
    // The helper (re)connected to the app, or the app quit.
    if (message.connected) linkUp(l);
    else ready = welcomed = false;
  } else if (message.type === 'welcome') {
    welcomed = true;
  } else if (message.type === 'probe' && Number.isInteger(message.tabId)) {
    chrome.tabs
      .sendMessage(message.tabId, { type: 'probe', x: Number(message.x), y: Number(message.y) })
      .catch(() => undefined);
  }
}

class NativeLink implements Link {
  private readonly port = chrome.runtime.connectNative(NATIVE_HOST_NAME);
  constructor() {
    this.port.onMessage.addListener((m: HostMessage) => onAppMessage(this, m));
    this.port.onDisconnect.addListener(() => {
      void chrome.runtime.lastError; // helper not installed, or it exited
      linkLost(this);
    });
  }
  send(message: object): void {
    this.port.postMessage(message);
  }
  close(): void {
    this.port.disconnect();
    linkLost(this);
  }
}

class WebSocketLink implements Link {
  private readonly ws: WebSocket;
  constructor(port: number) {
    this.ws = new WebSocket(`ws://127.0.0.1:${port}`);
    this.ws.onopen = () => {
      linkUp(this);
      // Traffic on an open WebSocket keeps the MV3 service worker alive (Chrome 116+); a
      // native-messaging port does that by itself.
      clearInterval(keepAlive);
      keepAlive = setInterval(() => this.ws.readyState === WebSocket.OPEN && this.ws.send('{"type":"ping"}'), 20_000);
    };
    this.ws.onmessage = (event) => {
      let message: HostMessage | null = null;
      try {
        message = JSON.parse(String(event.data)) as HostMessage;
      } catch {
        return;
      }
      onAppMessage(this, message);
    };
    this.ws.onclose = () => linkLost(this);
    this.ws.onerror = () => this.ws.close();
  }
  send(message: object): void {
    if (this.ws.readyState === WebSocket.OPEN) this.ws.send(JSON.stringify(message));
  }
  close(): void {
    this.ws.close();
    linkLost(this);
  }
}

async function connect(): Promise<void> {
  if (connecting || link || Date.now() < retryAt) return;
  // Set before the await, so overlapping calls can't open two links.
  connecting = true;
  let port: number | undefined;
  try {
    port = await devPort();
  } finally {
    connecting = false;
  }
  if (link) return;
  try {
    link = port === undefined ? new NativeLink() : new WebSocketLink(port);
  } catch {
    retryAt = Date.now() + retryMs;
    setTimeout(() => void connect(), retryMs);
    retryMs = Math.min(retryMs * 2, 10_000);
  }
}

// A (re)started app knows nothing yet: tell it which tab is visible in each window and
// have every page report its scroll state again.
async function resync(): Promise<void> {
  for (const tab of await chrome.tabs.query({})) {
    if (tab.id === undefined) continue;
    if (tab.active) emit({ type: 'focus', v: SYNC_PROTOCOL_VERSION, tabId: tab.id, windowId: tab.windowId });
    // Rejects for tabs without our content script (chrome:// pages, tabs opened before install).
    chrome.tabs.sendMessage(tab.id, { type: 'report' }).catch(() => undefined);
  }
  // Our own pages (the PDF viewer) aren't reached by tabs.sendMessage.
  chrome.runtime.sendMessage({ type: 'report' }).catch(() => undefined);
}

// ---- PDFs ------------------------------------------------------------------------------
// Chrome's built-in PDF viewer hides its scroll position from extensions, so PDFs open in
// ScrollPen's viewer (pdf/viewer.html), where ink can follow them exactly.

const PDF_VIEWER = chrome.runtime.getURL('pdf/viewer.html');

function isPdfUrl(url: string): boolean {
  try {
    const u = new URL(url);
    return ['http:', 'https:', 'file:'].includes(u.protocol) && u.pathname.toLowerCase().endsWith('.pdf');
  } catch {
    return false;
  }
}

/** Per tab: a PDF that should open in Chrome's own viewer next time (Back, or the user asked). */
const nativeKey = (tabId: number) => `pdf-native:${tabId}`;

async function routePdf(tabId: number, url: string): Promise<void> {
  const key = nativeKey(tabId);
  const { [key]: native } = await chrome.storage.session.get(key);
  if (native === url) {
    await chrome.storage.session.remove(key);
    return;
  }
  // Going Back from our viewer lands on this URL again: show Chrome's viewer then, not a loop.
  await chrome.storage.session.set({ [key]: url });
  await chrome.tabs.update(tabId, { url: `${PDF_VIEWER}#${encodeURIComponent(url)}` });
}

// PDFs that were already open in Chrome's viewer when ScrollPen was installed or updated
// would otherwise stay there until reopened.
chrome.runtime.onInstalled.addListener(() => {
  void chrome.tabs.query({}).then((tabs) => {
    for (const tab of tabs) if (tab.id !== undefined && tab.url && isPdfUrl(tab.url)) void routePdf(tab.id, tab.url);
  });
});

chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (!info.url) return;
  if (isPdfUrl(info.url)) void routePdf(tabId, info.url);
  else if (!info.url.startsWith(PDF_VIEWER)) void chrome.storage.session.remove(nativeKey(tabId));
});

chrome.runtime.onMessage.addListener((message: unknown, sender) => {
  const m = message as { type?: string; url?: string } | null;
  const tabId = sender.tab?.id;
  if (m?.type !== 'pdf-native' || typeof m.url !== 'string' || !isPdfUrl(m.url) || tabId === undefined) return;
  const url = m.url;
  void chrome.storage.session.set({ [nativeKey(tabId)]: url }).then(() => chrome.tabs.update(tabId, { url }));
});

function emit(message: SyncMessage): void {
  if (link && ready) {
    link.send(message);
    return;
  }
  // Keep only the newest few messages while the app isn't running.
  outbox.push(message);
  if (outbox.length > 50) outbox.splice(0, outbox.length - 50);
  void connect();
}

// Zoom per tab, so the app can convert the page's CSS pixels to screen pixels.
const zoomByTab = new Map<number, number>();

chrome.runtime.onConnect.addListener((port) => {
  if (port.name !== 'scrollpen-content') return;
  const tab = port.sender?.tab;
  const tabId = tab?.id;
  if (tabId === undefined || port.sender?.frameId !== 0) return;
  const windowId = tab?.windowId ?? -1;
  if (!zoomByTab.has(tabId)) {
    chrome.tabs.getZoom(tabId).then(
      (z) => zoomByTab.set(tabId, z),
      () => undefined,
    );
  }
  port.onMessage.addListener((report: ContentReport) => {
    if (report.type === 'scroll')
      emit({ ...report, tabId, windowId: windowByTab.get(tabId) ?? windowId, zoom: zoomByTab.get(tabId) ?? 1 });
    else emit({ ...report, tabId });
  });
});

// A tab dragged into another window keeps its page (and port) but changes window.
const windowByTab = new Map<number, number>();
chrome.tabs.onAttached.addListener((tabId, { newWindowId }) => windowByTab.set(tabId, newWindowId));

chrome.tabs.onActivated.addListener(({ tabId, windowId }) =>
  emit({ type: 'focus', v: SYNC_PROTOCOL_VERSION, tabId, windowId }),
);

chrome.tabs.onRemoved.addListener((tabId) => {
  zoomByTab.delete(tabId);
  windowByTab.delete(tabId);
  void chrome.storage.session.remove(nativeKey(tabId));
  emit({ type: 'navigate', v: SYNC_PROTOCOL_VERSION, tabId, docKey: null });
});

chrome.tabs.onZoomChange.addListener(({ tabId, newZoomFactor }) => {
  zoomByTab.set(tabId, newZoomFactor);
  emit({ type: 'zoom', v: SYNC_PROTOCOL_VERSION, tabId, zoom: newZoomFactor });
});

chrome.storage.onChanged.addListener((changes) => {
  if (changes.port) link?.close();
});

void connect();
