// Screenshots for the polish before/after review (not a test). Runs only when
// ../build/shots/.take exists; writes ../build/shots/current/<desktop|phone>/<NN-name>.png.
//   touch build/shots/.take && bin/hubbo web e2e --project=app --no-deps shots.spec
import { existsSync, mkdirSync } from 'node:fs'
import type { Page } from '@playwright/test'
import { buildFirmwareImage } from '../mock/firmware.ts'
import type { ResetFlags } from '../mock/handler.ts'
import { api, chooseFirmware, expect, open, test } from './hub.ts'

const ROOT = '../build/shots'
const DEVICES = {
  desktop: { viewport: { width: 1280, height: 800 } },
  phone: { viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true },
}

async function settle(page: Page) {
  await page.waitForTimeout(400)
  await expect(page.locator('.animate-pulse')).toHaveCount(0, { timeout: 5_000 }).catch(() => undefined)
  await page.waitForFunction(() => document.getAnimations().every((a) => a.playState !== 'running' || a.effect?.getTiming().iterations === Infinity))
}

/** Pages, by name and route. */
const SHOTS: [string, string][] = [
  ['01-status-overview', '/status/overview'],
  ['02-activities', '/'],
  ['03-activity-editor', '/activities/act_watch_apple_tv'],
  ['05-activity-new', '/activities/new'],
  ['06-remote-global', '/remote'],
  ['07-devices', '/devices'],
  ['09-device-detail', '/devices/dev_denon'],
  ['10-integrations', '/integrations'],
  ['12-settings-account', '/settings/account'],
  ['13-settings-tokens', '/settings/tokens'],
  ['15-settings-remote', '/settings/remote'],
  ['16-settings-network', '/settings/network'],
  ['17-status-backups', '/status/backups'],
  ['21-status-firmware', '/status/firmware'],
  ['23-status-diagnostics', '/status/diagnostics'],
  ['31-settings-reset', '/settings/reset'],
  ['26-setup-network', '/setup/network'],
  ['27-setup-remote', '/setup/remote'],
  ['28-setup-ha', '/setup/ha'],
  ['29-setup-done', '/setup/done'],
  ['30-not-found', '/nope'],
]

/** Full-screen pages (setup, login) have no main nav. */
const go = async (page: Page, hash: string) => {
  await page.goto(`/#${hash}`)
  await page.getByRole('heading').first().waitFor()
}

const dlg = (page: Page) => expect(page.getByRole('dialog')).toBeVisible()

/** `dialog`: shoot the viewport, not the full page. `reset`: the hub state to start from. */
type ShotOpts = { dialog?: boolean; reset?: ResetFlags }

const DIALOGS: [string, (page: Page) => Promise<void>, ShotOpts][] = [
  [
    '04-activity-add-step',
    async (page) => {
      await open(page, '/activities/act_watch_apple_tv')
      await page.getByRole('tab', { name: /Startup/ }).click()
      await page.getByRole('button', { name: 'Add step' }).click()
      await dlg(page)
    },
    { dialog: true },
  ],
  [
    '08-add-device',
    async (page) => {
      await open(page, '/devices')
      await page.getByRole('button', { name: 'Add device' }).first().click()
      await dlg(page)
    },
    { dialog: true },
  ],
  [
    '11-ble-pair',
    async (page) => {
      await open(page, '/devices/dev_apple_tv')
      await page.getByRole('button', { name: 'Pair again' }).click()
      await page.getByRole('dialog').getByRole('button', { name: 'Pair', exact: true }).click()
      await expect(page.getByTestId('bluetooth-state')).toContainText('Ready to pair')
    },
    { dialog: true },
  ],
  [
    '14-token-created',
    async (page) => {
      await open(page, '/settings/tokens')
      await page.getByRole('textbox').first().fill('Living room tablet')
      await page.getByRole('button', { name: /Create/ }).click()
      await dlg(page)
    },
    { dialog: true },
  ],
  [
    '18-restore-preview',
    async (page) => {
      await open(page, '/status/backups')
      const text = JSON.stringify(await api(page).get('/admin/backup'))
      await page.locator('input[type=file]').setInputFiles({ name: 'hubbo-backup.json', mimeType: 'application/json', buffer: Buffer.from(text) })
      await expect(page.getByText('Ready to restore')).toBeVisible()
    },
    {},
  ],
  [
    '19-reset-configuration',
    async (page) => {
      await open(page, '/settings/reset')
      await page.getByRole('button', { name: /Reset configuration/ }).first().click()
      await dlg(page)
    },
    { dialog: true },
  ],
  [
    '20-factory-reset',
    async (page) => {
      await open(page, '/settings/reset')
      await page.getByRole('button', { name: /Factory reset|Erase/ }).first().click()
      await dlg(page)
    },
    { dialog: true },
  ],
  [
    '22-firmware-chosen',
    async (page) => {
      await open(page, '/status/firmware')
      const { board } = await api(page).get('/admin/firmware')
      await chooseFirmware(page, await buildFirmwareImage({ board, version: 'v9.9.9' }))
      await expect(page.getByText(`Hubbo v9.9.9 for ${board}`)).toBeVisible()
    },
    {},
  ],
  [
    '32-activity-menu',
    async (page) => {
      await open(page, '/')
      await page.getByRole('button', { name: 'More for Watch Apple TV' }).click()
    },
    { dialog: true },
  ],
  [
    '33-activities-on',
    async (page) => {
      await api(page).post('/activities/act_watch_apple_tv/activate')
      await open(page, '/')
      await expect(page.getByRole('button', { name: /^Watch Apple TV/ })).toHaveAttribute('aria-pressed', 'true')
    },
    {},
  ],
  ['24-login', (page) => go(page, '/'), { reset: { requireAuth: true } }],
  ['25-setup-admin', (page) => go(page, '/'), { reset: { firstRun: true } }],
]

for (const [device, use] of Object.entries(DEVICES)) {
  test.describe(device, () => {
    test.skip(!existsSync(`${ROOT}/.take`), 'screenshots only on request')
    test.use({ ...use, colorScheme: 'light' })
    const dir = `${ROOT}/current/${device}`
    const snap = async (page: Page, name: string, dialog = false) => {
      mkdirSync(dir, { recursive: true })
      await settle(page)
      await page.screenshot({ path: `${dir}/${name}.png`, fullPage: !dialog })
    }
    /** One screenshot per test, so one failure doesn't lose the rest. */
    const shot = (name: string, run: (page: Page) => Promise<void>, opts: ShotOpts = {}) =>
      test.describe(name, () => {
        if (opts.reset) test.use({ reset: opts.reset })
        test(name, async ({ page }) => {
          await run(page)
          await snap(page, name, opts.dialog)
        })
      })

    for (const [name, hash] of SHOTS) shot(name, (page) => (hash.startsWith('/setup') ? go(page, hash) : open(page, hash)))
    for (const [name, run, opts] of DIALOGS) shot(name, run, opts)
  })
}
