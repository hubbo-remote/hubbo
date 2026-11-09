import type { Page } from '@playwright/test'
import { buttonLabel } from '../src/lib/buttons.ts'
import { api, expect, open, test } from './hub.ts'

async function bind(page: Page, button: string, event: string, device: string, command: string, key?: string) {
  await page.locator(`[data-key="${button}"]`).click()
  await page.getByRole('button', { name: `Bind ${buttonLabel(button)} ${event}` }).click()
  await page.getByLabel('Device').selectOption({ label: device })
  await page.getByLabel('Command').selectOption({ label: command })
  if (key) await page.getByLabel('Key', { exact: true }).selectOption(key)
  await page.getByRole('button', { name: 'Apply' }).click()
}

async function addStep(page: Page, device: string, command: string) {
  await page.getByRole('button', { name: 'Add step' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByLabel('Device').selectOption({ label: device })
  await dialog.getByLabel('Command').selectOption({ label: command })
  await dialog.getByRole('button', { name: 'Add step' }).click()
  await expect(dialog).toBeHidden()
}

test('create an activity: bindings on the remote, startup and shutdown', async ({ page }) => {
  await open(page, '/activities/new')
  await page.getByLabel('Activity name').fill('E2E Movie Night')

  // Key-like: down = key down; the matching key up is added automatically.
  await bind(page, 'up', 'down', 'Family Room Apple TV', 'Key down', 'ARROW_UP')
  await expect(page.getByText('Key up (arrow up)')).toBeVisible()
  // Click-or-hold on Info.
  await bind(page, 'info', 'tap', 'Family Room Apple TV', 'Key tap', 'PLAY_PAUSE')
  await bind(page, 'info', 'hold', 'Family Room Apple TV', 'Key tap', 'AC_HOME')
  // A Home Assistant binding.
  await bind(page, 'volume_up', 'down', 'Family Room Denon', 'Volume up')
  await expect(page.locator('[data-key="up"]')).toHaveAttribute('aria-label', /bound: down, up/)
  await expect(page.locator('[data-key="info"]')).toHaveAttribute('aria-label', /bound: tap, hold/)

  await page.getByRole('tab', { name: /Startup/ }).click()
  await addStep(page, 'Family Room LG TV', 'Power on')
  await page.getByRole('button', { name: 'Add wait' }).click()
  await addStep(page, 'Family Room Denon', 'Power on')
  await page.getByRole('button', { name: 'Move step 3 up' }).click()
  await expect(page.getByRole('list', { name: 'Startup steps' }).getByRole('listitem')).toHaveCount(3)

  await page.getByRole('tab', { name: /Shutdown/ }).click()
  await addStep(page, 'Family Room LG TV', 'Power off')

  await page.getByRole('button', { name: 'Validate' }).click()
  await expect(page.getByText('The hub accepts this activity')).toBeVisible()
  await page.getByRole('button', { name: /^Create/ }).click()
  await expect(page).toHaveURL(/#\/activities\/act_/)

  // What the hub stored.
  const id = page.url().split('/').pop()!
  const a = await api(page).get(`/activities/${id}`)
  expect(a.name).toBe('E2E Movie Night')
  // Revisions are config generations, so a new activity's revision is the hub's current one.
  await expect(page.getByText(`Revision ${a.revision}`, { exact: true })).toBeVisible()
  expect(Object.keys(a.bindings).sort()).toEqual(['info:hold', 'info:tap', 'up:down', 'up:up', 'volume_up:down'])
  expect(a.bindings['up:up'][0]).toMatchObject({ device_id: 'dev_apple_tv', command_id: 'key_up', params: { key: 'ARROW_UP' } })
  expect(a.startup.map((s: { command_id: string }) => s.command_id)).toEqual(['media_player.turn_on', 'media_player.turn_on', 'wait'])
  expect(a.shutdown).toHaveLength(1)
})

test('validation errors block saving and point at the button', async ({ page }) => {
  await open(page, '/activities/new')
  await expect(page.getByRole('button', { name: /^Create/ })).toBeDisabled()
  await expect(page.getByText('Name is required').first()).toBeVisible()
})

test('deleting the active activity switches to the next one', async ({ page }) => {
  // act_play_xbox is referenced by the global music button, so remove a fresh one instead.
  const hub = api(page)
  const first = (await hub.get('/activities')).activities[0].id
  const created = await hub.post('/activities', { name: 'E2E Temp', startup: [], shutdown: [], bindings: {} })
  expect(created.status()).toBe(201)
  const temp = await created.json()

  await open(page, '/activities')
  await page.getByRole('main').getByRole('button', { name: /Start/ }).last().click()
  await expect.poll(hub.activeActivity).toBe(temp.id)

  await page.getByRole('button', { name: 'More actions for E2E Temp' }).click()
  await page.getByRole('menuitem', { name: 'Delete' }).click()
  const dialog = page.getByRole('dialog')
  await expect(dialog).toContainText('switches to the next activity')
  await dialog.getByRole('button', { name: 'Delete' }).click()
  await expect(page.getByRole('main').getByRole('listitem').filter({ hasText: 'E2E Temp' })).toHaveCount(0)
  // It took the last list position, so the hub wraps to the first activity.
  await expect.poll(hub.activeActivity).toBe(first)
})

test('an activity referenced by a button cannot be deleted', async ({ page }) => {
  await open(page, '/activities')
  await page.getByRole('button', { name: 'More actions for Play Xbox' }).click()
  await page.getByRole('menuitem', { name: 'Delete' }).click()
  await page.getByRole('dialog').getByRole('button', { name: 'Delete' }).click()
  await expect(page.getByText('Could not delete')).toBeVisible()
  await expect(page.getByText(/Referenced by the global bindings/)).toBeVisible()
  await expect(page.getByRole('link', { name: /Play Xbox/ })).toBeVisible()
})
