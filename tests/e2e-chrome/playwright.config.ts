import { defineConfig } from '@playwright/test';

export default defineConfig({
  testDir: './specs',
  timeout: 30_000,
  // Each test launches its own browser with the extension; keep it modest.
  workers: 2,
  reporter: [['list'], ['html', { open: 'never', outputFolder: 'report' }]],
  use: {
    trace: 'retain-on-failure',
    screenshot: 'only-on-failure',
  },
});
