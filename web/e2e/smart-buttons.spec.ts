// Smart buttons on the Global remote: Light 1 takes a device, shows what it does (read-only) and saves with the
// page; back to None, it's an ordinary button again. Mock and hub-sim.
import type { Page } from '@playwright/test'
import type { SmartButtons } from '../src/api/types.ts'
import { emptySmartButtons } from '../src/lib/smartButtons.ts'
import { bind, openNewActivity, saveActivity as save } from './editor.ts'
import { api, expect, open, test } from './hub.ts'

const LIGHTS = 'dev_family_room_lights'

/** Start from four empty slots, or these. */
async function setSlots(page: Page, slots: Partial<SmartButtons['slots']> = {}) {
  const hub = api(page)
  const { revision } = await hub.get('/smart-buttons')
  const r = await hub.put('/smart-buttons', { revision, slots: { ...emptySmartButtons(revision).slots, ...slots } })
  expect(r.status(), await r.text()).toBe(200)
}

/** Give Global's Light 1 a Press, so a device's stripping shows. */
async function bindLight1(page: Page) {
  const hub = api(page)
  const off = { integration_id: 'system', device_id: null, command_id: 'off', params: {} }
  const g = await hub.get('/global-bindings')
  const r = await hub.put('/global-bindings', { revision: g.revision, bindings: { ...g.bindings, 'light_1:tap': [off] } })
  expect(r.status(), await r.text()).toBe(200)
}
const light1Bound = async (page: Page) => 'light_1:tap' in (await api(page).get('/global-bindings')).bindings

const key = (page: Page, id: string) => page.locator(`[data-key="${id}"]`).click()
const device = (page: Page) => page.getByLabel('Device', { exact: true })
const does = (page: Page) => page.getByRole('region', { name: 'What Light 1 does' })

test('Light 1 with a device shows what it does, and saves with the page', async ({ page }) => {
  await setSlots(page)
  await open(page, '/remote')
  await expect(page.getByLabel('Buttons for').locator('option', { hasText: 'Smart buttons' })).toHaveCount(0)
  await key(page, 'light_1')
  await device(page).selectOption({ label: 'Family Room Lights' })
  // Shown at once, before saving (the hub works it out), and + follows it.
  await expect(does(page)).toContainText('Turn on')
  await expect(does(page)).toContainText('Turn off')
  await expect(does(page)).toContainText('Change brightness')
  await expect(page.getByRole('button', { name: /Light 1 press$/ })).toHaveCount(0)
  await key(page, 'plus')
  await expect(page.getByRole('list', { name: 'Smart buttons' })).toContainText('Brightness up 10%')
  await save(page)
  expect((await api(page).get('/smart-buttons')).slots.light_1).toBe(LIGHTS)

  await page.reload()
  await key(page, 'light_1')
  await expect(device(page)).toHaveValue(LIGHTS)
  await expect(does(page)).toContainText('Change brightness')
})

test('a device clears the button’s global bindings, so None starts empty', async ({ page }) => {
  await setSlots(page)
  const hub = api(page)
  const off = { integration_id: 'system', device_id: null, command_id: 'off', params: {} }
  const g = await hub.get('/global-bindings')
  const r = await hub.put('/global-bindings', { revision: g.revision, bindings: { ...g.bindings, 'light_1:tap': [off], 'light_1:hold': [off] } })
  expect(r.status(), await r.text()).toBe(200)
  await open(page, '/remote')
  await key(page, 'light_1')
  await expect(page.getByRole('button', { name: 'Edit Light 1 press' })).toBeVisible()
  await device(page).selectOption({ label: 'Family Room Lights' })
  await expect(does(page)).toBeVisible()
  await save(page)
  expect((await hub.get('/smart-buttons')).slots.light_1).toBe(LIGHTS)
  expect(Object.keys((await hub.get('/global-bindings')).bindings).filter((k) => k.startsWith('light_1:'))).toEqual([])

  await device(page).selectOption({ label: 'None' })
  await expect(does(page)).toHaveCount(0)
  await expect(page.getByRole('button', { name: 'Bind Light 1 press' })).toBeVisible()
  await expect(page.getByRole('button', { name: 'Bind Light 1 long press' })).toBeVisible()
  await save(page)
  expect((await hub.get('/smart-buttons')).slots.light_1).toBeNull()
})

test('the first device takes + and −: their globals go, and the panel explains them', async ({ page }) => {
  await setSlots(page)
  const hub = api(page)
  const off = { integration_id: 'system', device_id: null, command_id: 'off', params: {} }
  const g = await hub.get('/global-bindings')
  const r = await hub.put('/global-bindings', { revision: g.revision, bindings: { ...g.bindings, 'plus:tap': [off], 'minus:hold': [off] } })
  expect(r.status(), await r.text()).toBe(200)
  await open(page, '/remote')
  await key(page, 'plus')
  await expect(page.getByRole('button', { name: 'Edit Plus press' })).toBeVisible()

  await key(page, 'light_1')
  await device(page).selectOption({ label: 'Family Room Lights' })
  await save(page)
  const left = Object.keys((await hub.get('/global-bindings')).bindings).filter((k) => /^(plus|minus):/.test(k))
  expect(left).toEqual([])

  await key(page, 'plus')
  await expect(page.getByText('Controls your smart buttons.')).toBeVisible()
  const card = page.getByRole('list', { name: 'Smart buttons' }).getByRole('listitem')
  await expect(card).toContainText('Light 1')
  await expect(card).toContainText('Family Room Lights')
  await expect(card).toContainText('Brightness up 10%')
  await expect(page.getByRole('button', { name: /Plus (press|long press)$/ })).toHaveCount(0)
  await key(page, 'minus')
  await expect(page.getByRole('list', { name: 'Smart buttons' })).toContainText('Brightness down 10%')

  // No slot devices: an ordinary button again.
  await setSlots(page)
  await page.reload()
  await key(page, 'plus')
  await expect(page.getByText('Controls your smart buttons.')).toHaveCount(0)
  await expect(page.getByRole('button', { name: 'Bind Plus press' })).toBeVisible()
})

test('an activity inherits the smart buttons, and can override them', async ({ page }) => {
  await setSlots(page, { light_1: LIGHTS })
  const id = await openNewActivity(page, 'Smart override')
  await key(page, 'light_1')
  await expect(page.getByText('Set on the Global remote.')).toBeVisible()
  const card = page.getByRole('list', { name: 'Smart buttons' })
  await expect(card).toContainText('Family Room Lights')
  await expect(card).toContainText('Turn on')

  await bind(page, 'light_1', 'press', 'Family Room Lights', 'Turn off')
  await save(page)
  expect(Object.keys((await api(page).get(`/activities/${id}`)).bindings).some((k) => k.startsWith('light_1:'))).toBe(true)
  await expect(page.getByText('Set on the Global remote.')).toHaveCount(0)
  await expect(page.getByText('This activity’s own, in place of the smart button.')).toBeVisible()
  await expect(page.getByRole('button', { name: 'Edit Light 1 press' })).toBeVisible()

  await key(page, 'plus')
  await expect(page.getByRole('list', { name: 'Smart buttons' })).toContainText('Brightness up 10%')
  await expect(page.getByRole('button', { name: 'Bind Plus press' })).toBeVisible()
})

test('a pick undone before saving changes nothing', async ({ page }) => {
  await setSlots(page)
  await bindLight1(page)
  await open(page, '/remote')
  await key(page, 'light_1')
  await device(page).selectOption({ label: 'Family Room Lights' })
  await expect(page.getByText('Unsaved changes')).toBeVisible()
  await device(page).selectOption({ label: 'None' })
  await expect(page.getByText('Unsaved changes')).toHaveCount(0)
  await expect(page.getByRole('button', { name: /^Save/ })).toBeDisabled()
  await expect(page.getByRole('button', { name: 'Edit Light 1 press' })).toBeVisible()
  expect(await light1Bound(page)).toBe(true)
})

test('a smart buttons conflict saves nothing else', async ({ page }) => {
  await setSlots(page)
  await bindLight1(page)
  await open(page, '/remote')
  await key(page, 'light_1')
  await device(page).selectOption({ label: 'Family Room Lights' })
  await setSlots(page, { switch_2: LIGHTS }) // someone else saves first
  await page.getByRole('button', { name: /^Save/ }).click()
  await expect(page.getByRole('dialog', { name: 'Changed somewhere else' })).toBeVisible()
  expect(await light1Bound(page)).toBe(true)
  expect((await api(page).get('/smart-buttons')).slots.light_1).toBeNull()
})
