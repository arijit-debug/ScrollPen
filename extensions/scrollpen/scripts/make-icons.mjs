// Draws ScrollPen's icons (icons/icon{16,32,48,128}.png): a blue rounded tile with a white
// pen and a wavy ink line. No dependencies: shapes are signed-distance functions sampled
// 4x4 per pixel, written as PNG with node:zlib. Run: npm run icons
import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { deflateSync } from 'node:zlib';

const out = join(dirname(fileURLToPath(import.meta.url)), '..', 'icons');
mkdirSync(out, { recursive: true });

const crcTable = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
const crc32 = (buf) => {
  let c = 0xffffffff;
  for (const b of buf) c = crcTable[(c ^ b) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
};
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(size, rgba) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0);
  ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // RGBA
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) {
    raw[y * (size * 4 + 1)] = 0; // no filter
    rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  }
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr),
    chunk('IDAT', deflateSync(raw, { level: 9 })),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// Shapes in unit coordinates (0..1), as signed distances (negative inside).
const roundedBox = (x, y, cx, cy, hw, hh, r) => {
  const qx = Math.abs(x - cx) - hw + r, qy = Math.abs(y - cy) - hh + r;
  return Math.hypot(Math.max(qx, 0), Math.max(qy, 0)) + Math.min(Math.max(qx, qy), 0) - r;
};
const segment = (x, y, ax, ay, bx, by, r) => {
  const dx = bx - ax, dy = by - ay;
  const t = Math.max(0, Math.min(1, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)));
  return Math.hypot(x - ax - t * dx, y - ay - t * dy) - r;
};
// The wavy ink line under the pen, as a polyline.
const wave = Array.from({ length: 33 }, (_, i) => {
  const t = i / 32;
  return [0.17 + t * 0.5, 0.79 + Math.sin(t * Math.PI * 2) * 0.045];
});
const waveDist = (x, y, r) => {
  let d = Infinity;
  for (let i = 1; i < wave.length; i++) d = Math.min(d, segment(x, y, ...wave[i - 1], ...wave[i], r));
  return d;
};

function shade(x, y, size) {
  // Thicker strokes at small sizes so the icon stays readable.
  const bold = size <= 16 ? 1.5 : size <= 32 ? 1.2 : 1;
  if (roundedBox(x, y, 0.5, 0.5, 0.47, 0.47, 0.2) > 0) return null;
  const pen = segment(x, y, 0.36, 0.64, 0.74, 0.26, 0.075 * bold);
  const tip = segment(x, y, 0.27, 0.73, 0.36, 0.64, 0.035 * bold);
  if (pen <= 0 || tip <= 0) return [255, 255, 255];
  if (waveDist(x, y, 0.03 * bold) <= 0) return [253, 216, 53];
  // Vertical gradient of the tile.
  const t = y;
  return [Math.round(37 + (29 - 37) * t), Math.round(99 + (78 - 99) * t), Math.round(235 + (216 - 235) * t)];
}

for (const size of [16, 32, 48, 128]) {
  const buf = Buffer.alloc(size * size * 4);
  const S = 4;
  for (let py = 0; py < size; py++) {
    for (let px = 0; px < size; px++) {
      let r = 0, g = 0, b = 0, a = 0;
      for (let sy = 0; sy < S; sy++) {
        for (let sx = 0; sx < S; sx++) {
          const c = shade((px + (sx + 0.5) / S) / size, (py + (sy + 0.5) / S) / size, size);
          if (!c) continue;
          r += c[0];
          g += c[1];
          b += c[2];
          a++;
        }
      }
      const o = (py * size + px) * 4;
      if (a) {
        buf[o] = Math.round(r / a);
        buf[o + 1] = Math.round(g / a);
        buf[o + 2] = Math.round(b / a);
      }
      buf[o + 3] = Math.round((a / (S * S)) * 255);
    }
  }
  writeFileSync(join(out, `icon${size}.png`), png(size, buf));
  console.log(`icons/icon${size}.png`);
}
