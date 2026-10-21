// Packs dist/ into zips (no dependencies: plain zips written with node:zlib):
//   release/scrollpen-<version>.zip               the file to upload to the Chrome Web Store
//   release/ScrollPen-for-friends-<version>.zip   a "ScrollPen" folder to load unpacked, with
//                                                 share/HOW TO INSTALL.txt inside it
// Run: npm run ext:pack
import { mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, relative, sep } from 'node:path';
import { fileURLToPath } from 'node:url';
import { crc32, deflateRawSync } from 'node:zlib';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const repo = join(root, '..', '..');
const dist = join(root, 'dist');
const { version } = JSON.parse(readFileSync(join(dist, 'manifest.json'), 'utf8'));
const outDir = join(repo, 'release');
mkdirSync(outDir, { recursive: true });

const files = [];
(function walk(dir) {
  for (const name of readdirSync(dir).sort()) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) walk(full);
    else files.push(full);
  }
})(dist);
if (files.some((f) => f.endsWith('.map'))) throw new Error('Source maps must not ship: build without --watch.');

/** Writes a zip of [name inside the zip, file on disk] pairs. */
function writeZip(target, entries) {
  const local = [];
  const central = [];
  let offset = 0;
  for (const [entryName, file] of entries) {
    const name = Buffer.from(entryName, 'utf8');
    const data = readFileSync(file);
    const packed = deflateRawSync(data, { level: 9 });
    const crc = crc32(data);
    const head = Buffer.alloc(30);
    head.writeUInt32LE(0x04034b50, 0);
    head.writeUInt16LE(20, 4); // version needed
    head.writeUInt16LE(0x0800, 6); // UTF-8 names
    head.writeUInt16LE(8, 8); // deflate
    head.writeUInt32LE(0x00210000, 10); // 1980-01-01 00:00, fixed so builds are reproducible
    head.writeUInt32LE(crc, 14);
    head.writeUInt32LE(packed.length, 18);
    head.writeUInt32LE(data.length, 22);
    head.writeUInt16LE(name.length, 26);
    local.push(head, name, packed);
    const dir = Buffer.alloc(46);
    dir.writeUInt32LE(0x02014b50, 0);
    dir.writeUInt16LE(20, 4);
    dir.writeUInt16LE(20, 6);
    dir.writeUInt16LE(0x0800, 8);
    dir.writeUInt16LE(8, 10);
    dir.writeUInt32LE(0x00210000, 12);
    dir.writeUInt32LE(crc, 16);
    dir.writeUInt32LE(packed.length, 20);
    dir.writeUInt32LE(data.length, 24);
    dir.writeUInt16LE(name.length, 28);
    dir.writeUInt32LE(offset, 42);
    central.push(dir, name);
    offset += head.length + name.length + packed.length;
  }
  const centralBuf = Buffer.concat(central);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(entries.length, 8);
  end.writeUInt16LE(entries.length, 10);
  end.writeUInt32LE(centralBuf.length, 12);
  end.writeUInt32LE(offset, 16);
  writeFileSync(target, Buffer.concat([...local, centralBuf, end]));
  console.log(`${relative(process.cwd(), target)} (${entries.length} files)`);
}

const inZip = (file) => relative(dist, file).split(sep).join('/');

writeZip(
  join(outDir, `scrollpen-${version}.zip`),
  files.map((f) => [inZip(f), f]),
);

// For friends: one "ScrollPen" folder that Chrome's "Load unpacked" takes as it is, with the
// install steps inside it (Chrome ignores files the manifest doesn't use).
writeZip(join(outDir, `ScrollPen-for-friends-${version}.zip`), [
  ['ScrollPen/HOW TO INSTALL.txt', join(repo, 'share', 'HOW TO INSTALL.txt')],
  ...files.map((f) => [`ScrollPen/${inZip(f)}`, f]),
]);
