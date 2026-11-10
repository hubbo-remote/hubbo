import { defineConfig, devices } from '@playwright/test'

// Runs in the pinned Playwright image (docker-compose `web-e2e`, via
// `bin/hubbo web e2e`). Starts its own Vite on :5181 unless
// HUBBO_E2E_BASE_URL points at an already running UI.
//   HUBBO_API_TARGET unset            TypeScript mock
//   HUBBO_API_TARGET=http://127.0.0.1:8780   hub-sim (`bin/hubbo sim serve [--auth]`)
const baseURL = process.env.HUBBO_E2E_BASE_URL ?? 'http://127.0.0.1:5181/'

export default defineConfig({
  testDir: 'e2e',
  // Every spec resets and then mutates one shared hub, so specs run one at a time.
  workers: 1,
  expect: { timeout: 8_000 },
  reporter: [['list'], ['html', { outputFolder: 'playwright-report', open: 'never' }]],
  use: { baseURL, trace: 'retain-on-failure', screenshot: 'only-on-failure', ...devices['Desktop Chrome'], viewport: { width: 1280, height: 900 } },
  // On hub-sim, first-run setup must see a hub with no admin, and the `seeded` specs a
  // hub started in a particular state (blocked config, storage down), so they run
  // before the rest, whose resets replace both.
  projects: [
    { name: 'first-run', testMatch: /(^|\/)setup\.spec\.ts$/ },
    { name: 'seeded', testMatch: /(^|\/)(blocked|storage-boot|credentials-corrupt)\.spec\.ts$/, dependencies: ['first-run'] },
    { name: 'app', testIgnore: /(^|\/)(setup|blocked|storage-boot|credentials-corrupt|storage-auth|board)\.spec\.ts$/, dependencies: ['seeded'] },
    // The main pages again on a phone (touch, 390×844), ordered like `app`.
    {
      name: 'phone',
      testMatch: /(^|\/)(dashboard|activate|activity|devices|remote|ble|a11y)\.spec\.ts$/,
      dependencies: ['seeded'],
      use: { ...devices['Desktop Chrome'], viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true },
    },
    // Leaves hub-sim --auth with storage down (it can't be brought back without a session), so it runs last.
    { name: 'last', testMatch: /(^|\/)storage-auth\.spec\.ts$/, dependencies: ['app', 'phone'] },
    // Read-only checks of a real board serving the UI (HUBBO_E2E_BASE_URL=http://<board>/): `--project board`.
    // It may type the real admin password, so nothing that records the page is kept.
    { name: 'board', testMatch: /(^|\/)board\.spec\.ts$/, use: { trace: 'off', screenshot: 'off', video: 'off' } },
  ],
  webServer: process.env.HUBBO_E2E_BASE_URL
    ? undefined
    : {
        command: 'node node_modules/vite/bin/vite.js',
        url: baseURL,
        timeout: 60_000,
        env: { ...(process.env as Record<string, string>), HUBBO_WEB_HOST: '127.0.0.1', HUBBO_WEB_PORT: '5181', HUBBO_MOCK_LATENCY: '0', HUBBO_MOCK_PAIR_MS: '0', HUBBO_MOCK_DISCOVER_MS: '0' },
      },
})
