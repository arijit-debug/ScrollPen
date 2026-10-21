// ScrollPen's PDF viewer. Chrome's built-in viewer doesn't let extensions see its scroll
// position, so the service worker opens PDFs here instead: an ordinary scrolling page the
// reporter can follow exactly, rendered locally with pdf.js.
import * as pdfjs from 'pdfjs-dist';
import { startReporter } from '../reporter';

const MAX_SCALE = 2.5;

interface Slot {
  page: pdfjs.PDFPageProxy;
  div: HTMLDivElement;
  renderedDpr: number;
  task: pdfjs.RenderTask | null;
}

/** The PDF's own URL: viewer.html#<encoded url>. */
const source = decodeURIComponent(location.hash.slice(1));
/** Only the addresses the service worker routes here (it already checks this). */
const sourceAllowed = (() => {
  try {
    return ['http:', 'https:', 'file:'].includes(new URL(source).protocol);
  } catch {
    return false;
  }
})();
const pagesEl = document.getElementById('pages') as HTMLDivElement;
const statusEl = document.getElementById('status') as HTMLDivElement;
let scale = 1;
const slots = new Map<Element, Slot>();

function fileName(url: string): string {
  try {
    return decodeURIComponent(new URL(url).pathname.split('/').pop() || url);
  } catch {
    return url;
  }
}

/** fetch() can't read file:// URLs; XMLHttpRequest can, given the extension's file access. */
function load(url: string): Promise<ArrayBuffer> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', url);
    xhr.responseType = 'arraybuffer';
    xhr.withCredentials = true;
    xhr.onload = () => {
      const ok = xhr.status === 0 || (xhr.status >= 200 && xhr.status < 300);
      if (ok && xhr.response) resolve(xhr.response as ArrayBuffer);
      else reject(new Error(`The server answered ${xhr.status}.`));
    };
    xhr.onerror = () =>
      reject(
        new Error(
          url.startsWith('file:')
            ? 'Chrome blocked reading the file. Turn on "Allow access to file URLs" for ScrollPen Browser Sync in chrome://extensions.'
            : 'The PDF could not be downloaded.',
        ),
      );
    xhr.send();
  });
}

async function render(slot: Slot): Promise<void> {
  const dpr = window.devicePixelRatio;
  if (slot.task || slot.renderedDpr === dpr) return;
  const viewport = slot.page.getViewport({ scale: scale * dpr });
  const canvas = document.createElement('canvas');
  canvas.width = Math.floor(viewport.width);
  canvas.height = Math.floor(viewport.height);
  const task = slot.page.render({ canvas, canvasContext: canvas.getContext('2d')!, viewport });
  slot.task = task;
  try {
    await task.promise;
    slot.div.replaceChildren(canvas);
    slot.renderedDpr = dpr;
  } catch {
    // Cancelled (scrolled away or re-rendering at a new zoom).
  } finally {
    if (slot.task === task) slot.task = null;
  }
}

function release(slot: Slot): void {
  slot.task?.cancel();
  slot.task = null;
  slot.div.replaceChildren();
  slot.renderedDpr = 0;
}

// Render pages near the viewport, drop the canvases of far-away ones (long PDFs).
const observer = new IntersectionObserver(
  (entries) => {
    for (const entry of entries) {
      const slot = slots.get(entry.target);
      if (!slot) continue;
      if (entry.isIntersecting) void render(slot);
      else release(slot);
    }
  },
  { rootMargin: '150% 0px' },
);

// Browser zoom changes devicePixelRatio, not the layout: re-render visible pages sharply.
function watchZoom(): void {
  matchMedia(`(resolution: ${window.devicePixelRatio}dppx)`).addEventListener(
    'change',
    () => {
      for (const slot of slots.values()) if (slot.renderedDpr) void render(slot);
      watchZoom();
    },
    { once: true },
  );
}

async function main(): Promise<void> {
  // This page can read any site with the user's cookies: never load anything else.
  if (!sourceAllowed) throw new Error('Only http, https and file addresses can be opened.');
  document.title = fileName(source);
  document.getElementById('native')!.addEventListener('click', () => {
    // The service worker lets this tab open the file in Chrome's own viewer once.
    void chrome.runtime.sendMessage({ type: 'pdf-native', url: source });
  });
  startReporter(`pdf:${source}`);

  pdfjs.GlobalWorkerOptions.workerSrc = chrome.runtime.getURL('pdf/pdf.worker.min.mjs');
  // The PDF is untrusted and this page can read any site and local files: only pages are
  // drawn (no pdf.js scripting or XFA forms), and the extension CSP forbids eval.
  const doc = await pdfjs.getDocument({
    data: await load(source),
    enableXfa: false,
    cMapUrl: chrome.runtime.getURL('pdf/cmaps/'),
    cMapPacked: true,
    standardFontDataUrl: chrome.runtime.getURL('pdf/standard_fonts/'),
    wasmUrl: chrome.runtime.getURL('pdf/wasm/'),
    iccUrl: chrome.runtime.getURL('pdf/iccs/'),
  }).promise;
  const info = await doc.getMetadata().catch(() => null);
  const title = (info?.info as { Title?: string } | undefined)?.Title?.trim();
  if (title) document.title = title;

  // Fit the first page to the window's width once. Later resizes don't reflow the pages,
  // so ink drawn on them stays on the same content.
  const first = await doc.getPage(1);
  const available = document.documentElement.clientWidth - 48;
  scale = Math.min(MAX_SCALE, Math.max(0.25, available / first.getViewport({ scale: 1 }).width));

  for (let n = 1; n <= doc.numPages; n++) {
    const page = n === 1 ? first : await doc.getPage(n);
    const viewport = page.getViewport({ scale });
    const div = document.createElement('div');
    div.className = 'page';
    div.style.width = `${Math.floor(viewport.width)}px`;
    div.style.height = `${Math.floor(viewport.height)}px`;
    pagesEl.append(div);
    slots.set(div, { page, div, renderedDpr: 0, task: null });
    observer.observe(div);
  }
  statusEl.remove();
  watchZoom();
}

main().catch((e: unknown) => {
  statusEl.textContent = `Could not open ${fileName(source)}: ${e instanceof Error ? e.message : String(e)}`;
});
