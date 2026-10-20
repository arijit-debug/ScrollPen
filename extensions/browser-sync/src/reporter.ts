import {
  SYNC_PROTOCOL_VERSION,
  type ContentReport,
  type ScrollMessage,
} from '@scrollpen/shared-types';

/**
 * Reports a page's scroll positions to the service worker, which forwards them to the
 * ScrollPen app. Used by the content script on web pages and by ScrollPen's PDF viewer.
 * `docKey` identifies the document the ink belongs to.
 */
export function startReporter(docKey: string): void {
  let port: chrome.runtime.Port | null = null;

  function send(report: ContentReport): void {
    try {
      if (!port) {
        port = chrome.runtime.connect({ name: 'scrollpen-content' });
        port.onDisconnect.addListener(() => {
          port = null;
        });
      }
      port.postMessage(report);
    } catch {
      // Extension was reloaded or disabled; this tab's script is orphaned.
      port = null;
    }
  }

  /** Short, stable CSS path for an inner scroller so the app can key its layer. */
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

  function measure(target: Element | null): Omit<ScrollMessage, 'tabId' | 'windowId' | 'zoom'> {
    const isDocument =
      !target || target === document.scrollingElement || target === document.documentElement || target === document.body;
    const base = {
      type: 'scroll' as const,
      v: SYNC_PROTOCOL_VERSION,
      docKey,
      visible: document.visibilityState === 'visible',
      // Layout viewport including scrollbars: the app places the page from this size
      // (visualViewport excludes classic scrollbars and pinch zoom).
      viewportWidth: window.innerWidth,
      viewportHeight: window.innerHeight,
      devicePixelRatio: window.devicePixelRatio,
      // Viewport origin on screen: window origin + browser chrome (tabs, address bar).
      screenX: window.screenX + (window.outerWidth - window.innerWidth) / 2,
      screenY: window.screenY + (window.outerHeight - window.innerHeight),
      title: document.title,
      ts: performance.timeOrigin + performance.now(),
    } satisfies Partial<ScrollMessage>;

    if (isDocument) {
      const se = document.scrollingElement ?? document.documentElement;
      return {
        ...base,
        scroller: 'document',
        scrollerRect: { x: 0, y: 0, width: base.viewportWidth, height: base.viewportHeight },
        scrollX: window.scrollX,
        scrollY: window.scrollY,
        scrollWidth: se.scrollWidth,
        scrollHeight: se.scrollHeight,
      };
    }

    const r = target.getBoundingClientRect();
    return {
      ...base,
      scroller: cssPath(target),
      scrollerRect: { x: r.x, y: r.y, width: r.width, height: r.height },
      scrollX: target.scrollLeft,
      scrollY: target.scrollTop,
      scrollWidth: target.scrollWidth,
      scrollHeight: target.scrollHeight,
    };
  }

  // An image opened in a tab: Chrome centres it and zooming moves it around the page, so
  // it is reported as its own "scroller" and the app anchors ink to it.
  const image =
    document.contentType.startsWith('image/') && document.images.length === 1 ? document.images[0] : null;

  function measureImage(img: HTMLImageElement): Omit<ScrollMessage, 'tabId' | 'windowId' | 'zoom'> {
    const r = img.getBoundingClientRect();
    return {
      ...measure(null),
      scroller: 'image',
      scrollerRect: { x: r.x, y: r.y, width: r.width, height: r.height },
      scrollX: 0,
      scrollY: 0,
      scrollWidth: img.naturalWidth,
      scrollHeight: img.naturalHeight,
      contentScale: img.naturalWidth > 0 ? r.width / img.naturalWidth : 1,
    };
  }

  // Coalesce scroll events to one report per frame per scroller.
  const pending = new Set<Element | null>();
  let frameRequested = false;

  function flush(): void {
    frameRequested = false;
    for (const target of pending) send(target && target === image ? measureImage(image) : measure(target));
    pending.clear();
  }

  function queue(target: Element | null): void {
    pending.add(target);
    // Anything that moves the page moves the image too.
    if (image) pending.add(image);
    if (!frameRequested) {
      frameRequested = true;
      requestAnimationFrame(flush);
    }
  }

  /**
   * Inner scrollers the app may have ink on. They move on screen when anything around them
   * scrolls or resizes, so their position is re-reported then too.
   */
  const watched = new Set<Element>();

  function queueWatched(): void {
    for (const el of watched) {
      if (el.isConnected) queue(el);
      else watched.delete(el);
    }
  }

  /** The nearest element at (x, y) that scrolls vertically, other than the page itself. */
  function scrollerAt(x: number, y: number): Element | null {
    for (let el = document.elementFromPoint(x, y); el; el = el.parentElement) {
      if (el === document.documentElement || el === document.body || el === document.scrollingElement) return null;
      const overflowY = getComputedStyle(el).overflowY;
      if ((overflowY === 'auto' || overflowY === 'scroll' || overflowY === 'overlay') && el.scrollHeight > el.clientHeight + 1)
        return el;
    }
    return null;
  }

  function reportAll(): void {
    send({ type: 'navigate', v: SYNC_PROTOCOL_VERSION, docKey });
    queue(null);
    queueWatched();
  }

  document.addEventListener(
    'scroll',
    (e) => {
      const target = e.target instanceof Element ? e.target : null;
      if (target && target !== document.scrollingElement) watched.add(target);
      queue(target);
      queueWatched();
    },
    { capture: true, passive: true },
  );
  window.addEventListener(
    'resize',
    () => {
      queue(null);
      queueWatched();
    },
    { passive: true },
  );
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') queue(null);
  });
  // Clicking an image toggles Chrome's shrink-to-fit, which resizes it without scrolling.
  if (image) new ResizeObserver(() => queue(image)).observe(image);
  // The app matches windows to tabs by title, so report title changes ("(2) Inbox").
  let lastTitle = document.title;
  new MutationObserver(() => {
    if (document.title === lastTitle) return;
    lastTitle = document.title;
    queue(null);
  }).observe(document.head ?? document.documentElement, { subtree: true, childList: true, characterData: true });

  chrome.runtime.onMessage.addListener((message: unknown) => {
    const m = message as { type?: string; x?: number; y?: number } | null;
    if (m?.type === 'report') {
      // The service worker (re)connected to the app: report again, so a freshly started app
      // knows each page without waiting for it to scroll.
      reportAll();
    } else if (m?.type === 'probe' && typeof m.x === 'number' && typeof m.y === 'number') {
      // The user started drawing here: report the element that would scroll the ink.
      const el = scrollerAt(m.x, m.y);
      if (el) {
        watched.add(el);
        queue(el);
      }
      queue(null);
    }
  });

  reportAll();
}
