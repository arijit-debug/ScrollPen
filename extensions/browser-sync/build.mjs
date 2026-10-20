// Bundles the extension into dist/ (load that folder via chrome://extensions > Load unpacked).
import { build, context } from 'esbuild';
import { cpSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(fileURLToPath(import.meta.url));
const dist = join(root, 'dist');
const watch = process.argv.includes('--watch');

rmSync(dist, { recursive: true, force: true });
mkdirSync(dist, { recursive: true });
cpSync(join(root, 'manifest.json'), join(dist, 'manifest.json'));

const options = {
  entryPoints: {
    content: join(root, 'src/content.ts'),
    background: join(root, 'src/background.ts'),
  },
  outdir: dist,
  bundle: true,
  format: 'iife',
  target: 'chrome116',
  sourcemap: 'linked',
  logLevel: 'info',
};

if (watch) {
  await (await context(options)).watch();
} else {
  await build(options);
}
