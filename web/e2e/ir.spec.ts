// Infrared devices: added by importing a profile file (Add device → Import a profile file), no
// pairing. Their commands bind to buttons and steps like any device's. hub-sim records what its IR
// emitter sent (GET /sim/ir) and takes button presses; the mock keeps the startup's run.
import type { Locator, Page } from '@playwright/test'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { addStep, bind, openNewActivity, saveActivity } from './editor.ts'
import { advance, api, control, expect, open, SIM_URL, test } from './hub.ts'

const NEC_TV = fileURLToPath(new URL('../../sim/fixtures/ir/nec-tv.json', import.meta.url))

type Upload = string | { name: string; mimeType: string; buffer: Buffer }

/** In the open Add device dialog: Import a profile file, and pick `file`. */
async function pickFile(page: Page, dialog: Locator, file: Upload) {
  const chooser = page.waitForEvent('filechooser')
  await dialog.getByRole('button', { name: /^Import a profile file/ }).click()
  await (await chooser).setFiles(file)
}

/** Open Add device and import `file`; returns the dialog. */
async function importFile(page: Page, file: Upload) {
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).first().click()
  const dialog = page.getByRole('dialog')
  await pickFile(page, dialog, file)
  return dialog
}

/** What hub-sim's emitter sent, as "device command". */
async function irSent(page: Page): Promise<string[]> {
  const ir = (await (await page.request.get(`${SIM_URL}/sim/ir`)).json()) as { sends: { device_id: string; command: string }[] }
  return ir.sends.map((s) => `${s.device_id} ${s.command}`)
}

test('import an infrared profile, bind its commands, and a startup sends one', async ({ page }) => {
  const dialog = await importFile(page, NEC_TV)
  await expect(dialog).toContainText('NEC TV')
  await expect(dialog).toContainText('Test · IR') // its integration's name
  await expect(dialog.getByLabel('Name')).toHaveValue('NEC TV') // the model is the default name
  await dialog.getByLabel('Name').fill('Den TV')
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(dialog.getByText('Added Den TV')).toBeVisible() // no pairing: the dialog stays open for the next one
  await dialog.getByRole('button', { name: 'Done' }).click()

  const { devices } = (await api(page).get('/devices')) as { devices: { id: string; name: string; integration_id: string }[] }
  const tv = devices.find((d) => d.name === 'Den TV')!
  expect(tv.integration_id).toBe('ir')
  await expect(page.locator(`a[href="#/devices/${tv.id}"]`).getByRole('img', { name: 'IR', exact: true })).toBeVisible()
  await expect(page.locator(`a[href="#/devices/${tv.id}"]`)).toContainText('Test NEC TV') // what it is, under the user's name

  // Its page: the profile's commands, nothing to pair.
  await open(page, `/devices/${tv.id}`)
  await expect(page.getByRole('heading', { name: 'Den TV' })).toBeVisible()
  await expect(page.locator('header').filter({ has: page.getByRole('heading', { name: 'Den TV' }) })).toContainText('Test NEC TV')
  await expect(page.getByRole('group', { name: 'Volume up', exact: true })).toBeVisible()
  await expect(page.getByTestId('bluetooth-state')).toHaveCount(0)

  // A button and a startup step, picked by the profile's command names.
  const id = await openNewActivity(page, 'IR e2e')
  await bind(page, 'volume_up', 'press', 'Den TV', 'Volume up')
  await page.getByRole('tab', { name: /Startup/ }).click()
  await addStep(page, 'Den TV', 'Power on')
  await saveActivity(page)
  const saved = (await api(page).get(`/activities/${id}`)) as { startup: { device_id: string; params: { command?: string } }[]; bindings: Record<string, { device_id: string; params: { command?: string } }[]> }
  expect(saved.startup).toEqual([expect.objectContaining({ device_id: tv.id, params: expect.objectContaining({ command: 'PowerOn' }) })])
  expect(Object.values(saved.bindings).flat()).toContainEqual(expect.objectContaining({ device_id: tv.id, params: expect.objectContaining({ command: 'VolumeUp' }) }))

  expect((await api(page).post(`/activities/${id}/activate`)).status()).toBe(200)
  if (SIM_URL) {
    await expect.poll(() => irSent(page)).toContain(`${tv.id} PowerOn`)
    await advance(page, 1_000)
    await control(page, 'remote/volume_up/tap')
    await expect.poll(() => irSent(page)).toContain(`${tv.id} VolumeUp`)
  } else {
    await expect.poll(async () => (await api(page).get('/activity-runs')).runs[0]).toMatchObject({ activity_id: id, steps: [{ device_id: tv.id, result: 'ok' }] })
  }
})

test('cancelling the file picker leaves the dialog open', async ({ page }) => {
  await open(page, '/devices')
  await page.getByRole('button', { name: 'Add device' }).first().click()
  const dialog = page.getByRole('dialog')
  // Playwright can't dismiss the OS picker; dismissing it fires this (bubbling) cancel on the input.
  await dialog.getByLabel('Profile file').dispatchEvent('cancel', { bubbles: true })
  await expect(dialog.getByRole('searchbox', { name: 'Search manufacturers' })).toBeVisible()
  // Esc still closes it.
  await page.keyboard.press('Escape')
  await expect(dialog).toBeHidden()
})

test('a file with a bad code, or one that isn’t a profile, says what’s wrong', async ({ page }) => {
  const doc = JSON.parse(readFileSync(NEC_TV, 'utf8'))
  doc.commands.VolumeUp.pronto = '0000 006D 0001 0000 0000 0010' // a zero duration
  const dialog = await importFile(page, { name: 'broken.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(doc)) })
  await dialog.getByRole('button', { name: 'Add device', exact: true }).click()
  await expect(dialog.getByText('This profile file has a problem')).toBeVisible()
  await expect(dialog.getByText('The code for “VolumeUp” isn’t valid.')).toBeVisible()

  await dialog.getByRole('button', { name: 'Back' }).click()
  await pickFile(page, dialog, { name: 'notes.txt', mimeType: 'text/plain', buffer: Buffer.from('not json') })
  await expect(dialog.getByText('notes.txt isn’t a profile file.')).toBeVisible()
  await expect(dialog.getByLabel('Name')).toHaveCount(0) // nothing to add
})
