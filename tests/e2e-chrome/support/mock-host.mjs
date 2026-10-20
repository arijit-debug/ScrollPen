// Stand-in for the ScrollPen desktop app's sync server. Used by the Playwright
// tests and, via `npm run mock-host`, for manual testing in real Chrome.
import { WebSocketServer } from 'ws';

/**
 * @param {number} port 0 picks a free port.
 * @param {{ log?: boolean }} [opts]
 */
export async function startMockHost(port = 0, opts = {}) {
  const messages = [];
  const waiters = new Set();

  const wss = new WebSocketServer({
    host: '127.0.0.1',
    port,
    // Same rule the real app will enforce: only Chrome/Edge extensions may connect,
    // so an ordinary web page can't talk to ScrollPen.
    verifyClient: ({ origin }) => (origin ?? '').startsWith('chrome-extension://'),
  });
  await new Promise((resolve) => wss.once('listening', resolve));

  wss.on('connection', (ws) => {
    ws.on('message', (raw) => {
      const msg = JSON.parse(String(raw));
      if (msg.type === 'ping') return;
      messages.push(msg);
      if (opts.log) console.log(JSON.stringify(msg));
      for (const w of waiters) {
        if (w.pred(msg)) {
          waiters.delete(w);
          w.resolve(msg);
        }
      }
    });
  });

  return {
    port: /** @type {import('node:net').AddressInfo} */ (wss.address()).port,
    messages,
    /** Resolves with the first message (past or future) matching pred. */
    waitFor(pred, timeoutMs = 5000) {
      const hit = messages.find(pred);
      if (hit) return Promise.resolve(hit);
      return new Promise((resolve, reject) => {
        const w = { pred, resolve };
        waiters.add(w);
        setTimeout(() => {
          if (waiters.delete(w)) reject(new Error(`mock host: no matching message within ${timeoutMs} ms`));
        }, timeoutMs);
      });
    },
    lastScroll() {
      return messages.findLast((m) => m.type === 'scroll');
    },
    clear() {
      messages.length = 0;
    },
    close: () =>
      new Promise((resolve) => {
        // The extension reconnects forever, so drop clients instead of waiting for them.
        for (const client of wss.clients) client.terminate();
        wss.close(resolve);
      }),
  };
}

if (import.meta.url === `file:///${process.argv[1]?.replace(/\\/g, '/')}`) {
  const port = Number(process.argv[2] ?? 47615);
  const host = await startMockHost(port, { log: true });
  console.log(`ScrollPen mock host listening on ws://127.0.0.1:${host.port} (Ctrl+C to stop)`);
}
