// Checks the browser link of the REAL running ScrollPen app (start the app first):
//  - the dev WebSocket port is closed (it is off unless the "devWebSocket" setting is on)
//  - ScrollPenHost.exe refuses to start for any extension but ScrollPen's
//  - a program that isn't ScrollPenHost.exe is refused by the app's pipe
//  - through the real helper: malformed and flooding messages don't hurt the app, which
//    still answers hello with welcome; an oversized message makes the helper quit.
// Moves no mouse or keyboard. Usage: node link-check.mjs [path\to\ScrollPenHost.exe]
import { spawn, execFileSync } from 'node:child_process';
import net from 'node:net';
import { join } from 'node:path';

const EXTENSION_ORIGIN = 'chrome-extension://egihgjpnabmnoljofdkeakgplgjeidai/';
const hostExe = process.argv[2] ?? join(process.env.LOCALAPPDATA, 'ScrollPen', 'NativeHost', 'ScrollPenHost.exe');
const sid = execFileSync('whoami', ['/user', '/fo', 'csv', '/nh']).toString().trim().split(',')[1].replaceAll('"', '');
const pipeName = `\\\\.\\pipe\\ScrollPen.Sync.${sid}`;

let failures = 0;
const check = (ok, what) => {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures++;
};
const appPid = () => {
  const out = execFileSync('tasklist', ['/fi', 'imagename eq ScrollPen.exe', '/fo', 'csv', '/nh']).toString();
  return out.includes('ScrollPen.exe') ? out.split(',')[1].replaceAll('"', '') : null;
};
const frame = (obj) => {
  const body = Buffer.from(typeof obj === 'string' ? obj : JSON.stringify(obj));
  const len = Buffer.alloc(4);
  len.writeUInt32LE(body.length);
  return Buffer.concat([len, body]);
};

/** Runs ScrollPenHost.exe like Chrome does and parses its framed output. */
function startHost(origin) {
  const child = spawn(hostExe, [origin, '--parent-window=0'], { stdio: ['pipe', 'pipe', 'inherit'] });
  const messages = [];
  const waiters = [];
  let buf = Buffer.alloc(0);
  child.stdout.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 4 && buf.length >= 4 + buf.readUInt32LE(0)) {
      const n = buf.readUInt32LE(0);
      const msg = JSON.parse(buf.subarray(4, 4 + n).toString());
      buf = buf.subarray(4 + n);
      messages.push(msg);
      for (const w of [...waiters]) if (w.pred(msg)) waiters.splice(waiters.indexOf(w), 1) && w.resolve(msg);
    }
  });
  const exited = new Promise((resolve) => child.on('exit', (code) => resolve(code)));
  return {
    child,
    exited,
    send: (obj) => child.stdin.write(frame(obj)),
    waitFor: (pred, ms = 5000) =>
      new Promise((resolve, reject) => {
        const hit = messages.find(pred);
        if (hit) return resolve(hit);
        waiters.push({ pred, resolve });
        setTimeout(() => reject(new Error('timeout')), ms);
      }),
  };
}

const startPid = appPid();
check(!!startPid, 'ScrollPen.exe is running');
if (!startPid) process.exit(1);

// 1. Dev WebSocket off.
const portOpen = await new Promise((resolve) => {
  const s = net.connect(47615, '127.0.0.1');
  s.once('connect', () => (s.destroy(), resolve(true)));
  s.once('error', () => resolve(false));
});
check(!portOpen, 'nothing listens on 127.0.0.1:47615 (dev WebSocket is off)');

// 2. Wrong extension.
const wrong = startHost('chrome-extension://aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/');
check((await wrong.exited) === 1, 'helper refuses to run for another extension');

// 3. Impostor client on the pipe: accepted by Windows (same user), then dropped by the app
// because it isn't ScrollPenHost.exe. It must never receive anything.
const impostor = await new Promise((resolve) => {
  const s = net.connect(pipeName);
  let got = 0;
  s.on('data', (d) => (got += d.length));
  s.on('connect', () => s.write(JSON.stringify({ type: 'hello', v: 1 })));
  s.on('error', () => {});
  s.on('close', () => resolve({ closed: true, got }));
  setTimeout(() => (s.destroy(), resolve({ closed: false, got })), 3000);
});
check(impostor.closed && impostor.got === 0, 'app drops a pipe client that is not ScrollPenHost.exe');

// 4. The real helper.
const host = startHost(EXTENSION_ORIGIN);
await host.waitFor((m) => m.type === 'host' && m.connected === true);
check(true, 'helper verified the app and connected');

const malformed = [
  { v: 1, type: 'focus', tabId: 'x', windowId: 1 },
  { v: 1, type: 'scroll', tabId: 1, docKey: 'a', scrollerRect: 5 },
  { v: 1, type: 'scroll', tabId: 1, docKey: 'a', scrollerRect: { x: 'no' } },
  { v: 1, type: 'scroll', tabId: 1, docKey: 7 },
  { v: 1, type: 'scroll', tabId: 1e12, docKey: 'a' },
  { v: 1, type: 'scroll', tabId: 1, docKey: 'a', title: { t: 1 } },
  { v: 1, type: 'zoom', tabId: 1, zoom: 'big' },
  { v: 1, type: 'navigate', tabId: 1 },
  { v: '1', type: 'scroll' },
  { v: 1, type: 42 },
  [1, 2, 3],
  'null',
  '{"v":1,"type":"scroll","tabId":1,"docKey":"a","scrollY":1e999}',
  'not json at all',
];
for (const m of malformed) host.send(m);
for (let i = 0; i < 3000; i++)
  host.send({ v: 1, type: 'scroll', tabId: 100000 + i, docKey: `https://x/${i}`, scroller: `s${i % 500}`, scrollY: i });
host.send({ v: 1, type: 'hello', extVersion: 'link-check', userAgent: 'node' });
const welcome = await host.waitFor((m) => m.type === 'welcome', 10000).catch(() => null);
check(!!welcome, 'app survived malformed + 3000 flood messages and answered hello with welcome');
check(appPid() === startPid, 'ScrollPen.exe is the same process (no crash)');

// 5. Oversized message: the helper quits instead of buffering it.
host.child.stdin.write(Buffer.from([0x00, 0x00, 0x10, 0x00])); // 1 MiB length prefix
check((await host.exited) === 1, 'helper exits on an oversized message');
check(appPid() === startPid, 'ScrollPen.exe still running');

console.log(failures ? `${failures} check(s) failed` : 'All link checks passed');
process.exit(failures ? 1 : 0);
