import { defineConfig, devices } from '@playwright/test'

// Runs in the pinned Playwright image (docker-compose `web-e2e`, via
// `bin/hubbo web e2e`). Starts its own Vite unless HUBBO_E2E_BASE_URL points
// at an already running UI.
//   HUBBO_API_TARGET unset            TypeScript mock
//   HUBBO_API_TARGET=http://127.0.0.1:8780   hub-sim (`bin/hubbo sim serve [--auth]`)
/** Against hub-sim the phone project is left out (layout and touch are the mock's job), and `noauth` is added. */
const onHubSim = !!process.env.HUBBO_API_TARGET && process.env.HUBBO_API_TARGET !== 'mock'
// A mock run and a hub-sim run can go at once: each has its own Vite port, cache and output dirs.
const target = onHubSim ? 'sim' : 'mock'
const port = onHubSim ? 5182 : 5181
const baseURL = process.env.HUBBO_E2E_BASE_URL ?? `http://127.0.0.1:${port}/`
// On hub-sim, first-run setup must see a hub with no admin, and the `seeded` specs a
// hub started in a particular state (blocked config, storage down), so they run
// before the rest, whose resets replace both. The mock resets per test: no order needed.
const after = (project: string) => (onHubSim ? [project] : [])

export default defineConfig({
  testDir: 'e2e',
  outputDir: `test-results/${target}`,
  // hub-sim: every spec resets and then mutates one shared hub, so specs run one at a time.
  // The mock: each worker has a hub of its own (MOCK_HUB_HEADER, e2e/hub.ts).
  workers: onHubSim ? 1 : 4,
  fullyParallel: !onHubSim,
  expect: { timeout: 8_000 },
  reporter: [['list'], ['html', { outputFolder: `playwright-report/${target}`, open: 'never' }]],
  use: { baseURL, trace: 'retain-on-failure', screenshot: 'only-on-failure', ...devices['Desktop Chrome'], viewport: { width: 1280, height: 900 } },
  projects: [
    { name: 'first-run', testMatch: /(^|\/)setup\.spec\.ts$/ },
    { name: 'seeded', testMatch: /(^|\/)(blocked|storage-boot)\.spec\.ts$/, dependencies: after('first-run') },
    { name: 'app', testIgnore: /(^|\/)(setup|blocked|storage-boot|board)\.spec\.ts$/, dependencies: after('seeded') },
    ...(onHubSim
      ? [
          // hub-sim without auth: only the specs that behave differently there (`--project=noauth`).
          // Under --auth its tests skip themselves (hub.ts); app already ran them.
          { name: 'noauth', testMatch: /(^|\/)(firmware|tokens)\.spec\.ts$/ },
        ]
      : [
          // Layout and touch on a phone (390×844); desktop covers the flows. Mock only.
          {
            name: 'phone',
            testMatch: /(^|\/)(activate|status|remote|remote-page|ble|a11y)\.spec\.ts$/,
            use: { ...devices['Desktop Chrome'], viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true },
          },
        ]),
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
        env: {
          ...(process.env as Record<string, string>),
          HUBBO_WEB_HOST: '127.0.0.1',
          HUBBO_WEB_PORT: String(port),
          HUBBO_VITE_CACHE_DIR: `node_modules/.vite-e2e-${target}`,
          HUBBO_MOCK_LATENCY: '0',
          HUBBO_MOCK_PAIR_MS: '0',
          HUBBO_MOCK_DISCOVER_MS: '0',
          HUBBO_MOCK_TIME_SCALE: '0',
        },
      },
})
