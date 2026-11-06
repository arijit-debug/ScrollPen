// Bundles the extension into dist/ (load that folder via chrome://extensions > Load unpacked).
import { build, context } from 'esbuild';
import { cpSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, 'dist');
const watch = process.argv.includes('--watch');

rmSync(dist, { recursive: true, force: true });
mkdirSync(join(dist, 'pdf'), { recursive: true });
mkdirSync(join(dist, 'options'), { recursive: true });
cpSync(join(root, 'manifest.json'), join(dist, 'manifest.json'));
cpSync(join(root, 'icons'), join(dist, 'icons'), { recursive: true });
cpSync(join(root, 'src/pdf/viewer.html'), join(dist, 'pdf/viewer.html'));
cpSync(join(root, 'src/options/options.html'), join(dist, 'options/options.html'));

// pdf.js renders PDFs locally: its worker, fonts, character maps and image decoders ship
// inside the extension, nothing is downloaded at runtime.
const pdfjs = dirname(createRequire(import.meta.url).resolve('pdfjs-dist/package.json'));
cpSync(join(pdfjs, 'build/pdf.worker.min.mjs'), join(dist, 'pdf/pdf.worker.min.mjs'));
for (const dir of ['cmaps', 'standard_fonts', 'wasm', 'iccs']) {
  if (existsSync(join(pdfjs, dir))) cpSync(join(pdfjs, dir), join(dist, 'pdf', dir), { recursive: true });
}

// Licences of the open-source code bundled into ScrollPen (their terms require shipping them).
const freehand = join(root, '..', '..', 'node_modules', 'perfect-freehand');
writeFileSync(
  join(dist, 'THIRD_PARTY_NOTICES.txt'),
  [
    'ScrollPen includes the following open-source software.',
    '',
    'pdf.js (https://github.com/mozilla/pdf.js): Apache License 2.0, full text below.',
    'Its character maps, standard fonts, image decoders and colour profiles in pdf/cmaps, pdf/standard_fonts,',
    'pdf/wasm and pdf/iccs come with their own licence files (LICENSE*) in those folders.',
    '',
    'perfect-freehand (https://github.com/steveruizok/perfect-freehand): MIT License, full text below.',
    '',
    '='.repeat(78),
    'pdf.js',
    '='.repeat(78),
    readFileSync(join(pdfjs, 'LICENSE'), 'utf8'),
    '='.repeat(78),
    'perfect-freehand',
    '='.repeat(78),
    readFileSync(join(freehand, 'LICENSE'), 'utf8'),
  ].join('\n'),
);

const common = {
  outdir: dist,
  bundle: true,
  target: 'chrome123',
  // Source maps only while developing (--watch); release builds ship just the code.
  sourcemap: watch ? 'linked' : false,
  logLevel: 'info',
};
const scripts = {
  ...common,
  entryPoints: {
    content: join(root, 'src/content.ts'),
    background: join(root, 'src/background.ts'),
    'options/options': join(root, 'src/options/options.ts'),
  },
  format: 'iife',
};
// pdf.js is an ES module (it uses import.meta), so the viewer is too.
const viewer = {
  ...common,
  entryPoints: { 'pdf/viewer': join(root, 'src/pdf/viewer.ts') },
  format: 'esm',
};

if (watch) {
  await (await context(scripts)).watch();
  await (await context(viewer)).watch();
} else {
  await build(scripts);
  await build(viewer);
}
