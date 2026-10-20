import {
  SYNC_DEFAULT_PORT,
  SYNC_PROTOCOL_VERSION,
  type ContentReport,
  type SyncMessage,
} from '@scrollpen/shared-types';

// The desktop app listens on loopback only. The port can be overridden
// (chrome.storage.local "port") so tests and dev builds don't collide.
let socket: WebSocket | null = null;
let retryMs = 500;
let keepAlive: ReturnType<typeof setInterval> | undefined;
const outbox: SyncMessage[] = [];

async function hostUrl(): Promise<string> {
  const { port } = await chrome.storage.local.get('port');
  return `ws://127.0.0.1:${typeof port === 'number' ? port : SYNC_DEFAULT_PORT}`;
}

async function connect(): Promise<void> {
  if (socket && socket.readyState <= WebSocket.OPEN) return;
  const ws = new WebSocket(await hostUrl());
  socket = ws;

  ws.onopen = () => {
    retryMs = 500;
    ws.send(
      JSON.stringify({
        type: 'hello',
        v: SYNC_PROTOCOL_VERSION,
        extVersion: chrome.runtime.getManifest().version,
        userAgent: navigator.userAgent,
      } satisfies SyncMessage),
    );
    for (const m of outbox.splice(0)) ws.send(JSON.stringify(m));
    // An open WebSocket with traffic keeps the MV3 service worker alive (Chrome 116+).
    clearInterval(keepAlive);
    keepAlive = setInterval(() => ws.readyState === WebSocket.OPEN && ws.send('{"type":"ping"}'), 20_000);
  };

  ws.onclose = () => {
    clearInterval(keepAlive);
    if (socket === ws) socket = null;
    setTimeout(() => void connect(), retryMs);
    retryMs = Math.min(retryMs * 2, 10_000);
  };

  ws.onerror = () => ws.close();
}

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
    if (report.type === 'scroll') emit({ ...report, tabId, windowId, zoom: zoomByTab.get(tabId) ?? 1 });
    else emit({ ...report, tabId });
  });
});

chrome.tabs.onActivated.addListener(({ tabId, windowId }) =>
  emit({ type: 'focus', v: SYNC_PROTOCOL_VERSION, tabId, windowId }),
);

chrome.tabs.onRemoved.addListener((tabId) => {
  zoomByTab.delete(tabId);
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
