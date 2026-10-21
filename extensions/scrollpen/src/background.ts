// ScrollPen's service worker: the toolbar button and keyboard shortcuts, screenshots,
// and opening PDFs in ScrollPen's viewer. It makes no network requests of its own.
import { loadSettings } from './ink/storage';

const VIEWER = chrome.runtime.getURL('pdf/viewer.html');
const DEFAULT_TITLE = chrome.runtime.getManifest().action?.default_title ?? 'ScrollPen';
const CANT_DRAW ="ScrollPen can't draw on this page (Chrome doesn't allow extensions on its own pages).";

/** Sends a message to the drawing layer of a tab, starting ScrollPen there first if needed. */
async function tellTab(tab: chrome.tabs.Tab, message: { type: string; name?: string }): Promise<void> {
  const tabId = tab.id;
  if (tabId === undefined) return;
  // ScrollPen's own pages (the PDF viewer) aren't reached by tabs.sendMessage.
  if (tab.url?.startsWith(VIEWER)) {
    await chrome.runtime.sendMessage({ ...message, tabId }).catch(() => undefined);
    return;
  }
  try {
    await chrome.tabs.sendMessage(tabId, message);
    return;
  } catch {
    // No ScrollPen on this tab yet (opened before ScrollPen was installed), or it can't run here.
  }
  try {
    await chrome.scripting.executeScript({ target: { tabId }, files: ['content.js'] });
    await chrome.tabs.sendMessage(tabId, message);
  } catch {
    await chrome.action.setBadgeText({ tabId, text: '!' });
    await chrome.action.setBadgeBackgroundColor({ tabId, color: '#D32F2F' });
    await chrome.action.setTitle({ tabId, title: CANT_DRAW });
  }
}

chrome.action.onClicked.addListener((tab) => void tellTab(tab, { type: 'toggle' }));

const COMMANDS = new Set(['clear', 'toggle-ink', 'screenshot', 'toggle-mode']);
chrome.commands.onCommand.addListener((name, tab) => {
  if (tab && COMMANDS.has(name)) void tellTab(tab, { type: 'command', name });
});

// For the automated tests: what clicking the toolbar button does, for a given tab.
(globalThis as { scrollpenToggle?: (tabId: number) => Promise<void> }).scrollpenToggle = async (tabId) =>
  tellTab(await chrome.tabs.get(tabId), { type: 'toggle' });

chrome.runtime.onMessage.addListener((message: unknown, sender, sendResponse) => {
  // Only ScrollPen's own scripts can send these (no page or other extension can).
  if (sender.id !== chrome.runtime.id) return;
  const m = message as { type?: string; url?: string } | null;
  const tab = sender.tab;
  if (m?.type === 'capture' && tab?.windowId !== undefined && tab.active) {
    chrome.tabs.captureVisibleTab(tab.windowId, { format: 'png' }).then(
      (dataUrl) => sendResponse({ dataUrl }),
      () => sendResponse({}),
    );
    return true; // answers asynchronously
  }
  if (m?.type === 'options') {
    void chrome.runtime.openOptionsPage();
  } else if (m?.type === 'pdf-native' && typeof m.url === 'string' && isPdfUrl(m.url) && tab?.id !== undefined) {
    // "Chrome viewer" in ScrollPen's PDF viewer: show this file in Chrome's own viewer once.
    const tabId = tab.id;
    const url = m.url;
    void chrome.storage.session.set({ [nativeKey(tabId)]: url }).then(() => chrome.tabs.update(tabId, { url }));
  }
  return undefined;
});

// ---- PDFs ------------------------------------------------------------------------------
// Chrome's built-in PDF viewer doesn't let extensions draw on it, so PDFs open in
// ScrollPen's viewer (pdf/viewer.html), which renders them on this computer with pdf.js.

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
  if (!(await loadSettings()).openPdfs) return;
  const key = nativeKey(tabId);
  const { [key]: native } = await chrome.storage.session.get(key);
  if (native === url) {
    await chrome.storage.session.remove(key);
    return;
  }
  // Going Back from our viewer lands on this URL again: show Chrome's viewer then, not a loop.
  await chrome.storage.session.set({ [key]: url });
  await chrome.tabs.update(tabId, { url: `${VIEWER}#${encodeURIComponent(url)}` });
}

// PDFs already open in Chrome's viewer when ScrollPen is installed or updated would
// otherwise stay there until reopened.
chrome.runtime.onInstalled.addListener(() => {
  void chrome.tabs.query({}).then((tabs) => {
    for (const tab of tabs) if (tab.id !== undefined && tab.url && isPdfUrl(tab.url)) void routePdf(tab.id, tab.url);
  });
});

chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (!info.url) return;
  // A new page in this tab: forget the "can't draw here" mark.
  void chrome.action.setBadgeText({ tabId, text: '' }).catch(() => undefined);
  void chrome.action.setTitle({ tabId, title: DEFAULT_TITLE }).catch(() => undefined);
  if (isPdfUrl(info.url)) void routePdf(tabId, info.url);
  else if (!info.url.startsWith(VIEWER)) void chrome.storage.session.remove(nativeKey(tabId));
});

chrome.tabs.onRemoved.addListener((tabId) => void chrome.storage.session.remove(nativeKey(tabId)));
