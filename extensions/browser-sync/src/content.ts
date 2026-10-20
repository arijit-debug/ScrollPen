import {
  SYNC_PROTOCOL_VERSION,
  type ContentReport,
  type ScrollMessage,
} from '@scrollpen/shared-types';

const docKey = location.origin + location.pathname + location.search;

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

function measure(target: Element | null): Omit<ScrollMessage, 'tabId'> {
  const isDocument =
    !target || target === document.scrollingElement || target === document.documentElement || target === document.body;
  const vv = window.visualViewport;
  const base = {
    type: 'scroll' as const,
    v: SYNC_PROTOCOL_VERSION,
    docKey,
    viewportWidth: vv?.width ?? window.innerWidth,
    viewportHeight: vv?.height ?? window.innerHeight,
    devicePixelRatio: window.devicePixelRatio,
    // Viewport origin on screen: window origin + browser chrome (tabs, address bar).
    screenX: window.screenX + (window.outerWidth - window.innerWidth) / 2,
    screenY: window.screenY + (window.outerHeight - window.innerHeight),
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

// Coalesce scroll events to one report per frame per scroller.
const pending = new Set<Element | null>();
let frameRequested = false;

function flush(): void {
  frameRequested = false;
  for (const target of pending) send(measure(target));
  pending.clear();
}

function queue(target: Element | null): void {
  pending.add(target);
  if (!frameRequested) {
    frameRequested = true;
    requestAnimationFrame(flush);
  }
}

document.addEventListener(
  'scroll',
  (e) => queue(e.target instanceof Element ? e.target : null),
  { capture: true, passive: true },
);
window.addEventListener('resize', () => queue(null), { passive: true });
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') queue(null);
});

send({ type: 'navigate', v: SYNC_PROTOCOL_VERSION, docKey });
queue(null);
