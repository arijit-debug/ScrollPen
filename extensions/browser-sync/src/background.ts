import {
  SYNC_DEFAULT_PORT,
  SYNC_PROTOCOL_VERSION,
  type ContentReport,
  type HostMessage,
  type SyncMessage,
} from '@scrollpen/shared-types';

// The desktop app listens on loopback only. The port can be overridden
// (chrome.storage.local "port") so tests and dev builds don't collide.
let socket: WebSocket | null = null;
let connecting = false;
let retryMs = 500;
// After a failed attempt (the app isn't running) don't try again before this time. Page
// reports arriving meanwhile wait in the outbox; each attempt that fails is logged by
// Chrome, so retrying on every report would flood the extension's error list.
let retryAt = 0;
let keepAlive: ReturnType<typeof setInterval> | undefined;
const outbox: SyncMessage[] = [];

async function hostUrl(): Promise<string> {
  const { port } = await chrome.storage.local.get('port');
  return `ws://127.0.0.1:${typeof port === 'number' ? port : SYNC_DEFAULT_PORT}`;
}

async function connect(): Promise<void> {
  if (connecting || (socket && socket.readyState <= WebSocket.OPEN) || Date.now() < retryAt) return;
  // Set before the await, so overlapping calls can't open two connections.
  connecting = true;
  let ws: WebSocket;
  try {
    ws = new WebSocket(await hostUrl());
  } finally {
    connecting = false;
  }
  socket = ws;

  ws.onopen = () => {
    retryMs = 500;
    retryAt = 0;
    ws.send(
      JSON.stringify({
        type: 'hello',
        v: SYNC_PROTOCOL_VERSION,
        extVersion: chrome.runtime.getManifest().version,
        userAgent: navigator.userAgent,
      } satisfies SyncMessage),
    );
    for (const m of outbox.splice(0)) ws.send(JSON.stringify(m));
    void resync();
    // An open WebSocket with traffic keeps the MV3 service worker alive (Chrome 116+).
    clearInterval(keepAlive);
    keepAlive = setInterval(() => ws.readyState === WebSocket.OPEN && ws.send('{"type":"ping"}'), 20_000);
  };

  ws.onmessage = (event) => {
    let message: HostMessage;
    try {
      message = JSON.parse(String(event.data)) as HostMessage;
    } catch {
      return;
    }
    if (message?.type === 'probe' && message.v === SYNC_PROTOCOL_VERSION && Number.isInteger(message.tabId)) {
      chrome.tabs
        .sendMessage(message.tabId, { type: 'probe', x: Number(message.x), y: Number(message.y) })
        .catch(() => undefined);
    }
  };

  ws.onclose = () => {
    clearInterval(keepAlive);
    if (socket === ws) socket = null;
    retryAt = Date.now() + retryMs;
    setTimeout(() => void connect(), retryMs);
    retryMs = Math.min(retryMs * 2, 10_000);
  };

  ws.onerror = () => ws.close();
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
  if (socket?.readyState === WebSocket.OPEN) {
    socket.send(JSON.stringify(message));
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
  if (changes.port) socket?.close();
});

void connect();
