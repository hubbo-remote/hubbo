// Activity editor steps shared by the specs (selectors in one place).
import type { Page } from '@playwright/test'
import { buttonLabel } from '../src/lib/buttons.ts'
import { api, expect, open } from './hub.ts'

/** Create an empty activity through the API and open it in the editor; returns its id. */
export async function openNewActivity(page: Page, name: string): Promise<string> {
  const r = await api(page).post('/activities', { name, startup: [], shutdown: [], bindings: {} })
  expect(r.status()).toBe(201)
  const { id } = (await r.json()) as { id: string }
  await open(page, `/activities/${id}`)
  await expect(page.getByLabel('Activity name')).toHaveValue(name)
  return id
}

/** Save the open activity and wait for the hub to take it. */
export async function saveActivity(page: Page) {
  await page.getByRole('button', { name: /^Save/ }).click()
  await expect(page.getByText('Saved', { exact: true }).first()).toBeVisible()
}

/** Select a button on the remote picture and bind one of its events. */
export async function bind(page: Page, button: string, event: string, device: string, command: string, key?: string) {
  await page.locator(`[data-key="${button}"]`).click()
  await page.getByRole('button', { name: `Bind ${buttonLabel(button)} ${event}` }).click()
  await page.getByLabel('Device').selectOption({ label: device })
  await page.getByLabel('Command', { exact: true }).selectOption({ label: command })
  if (key) await page.getByLabel('Key', { exact: true }).selectOption(key)
  await page.getByRole('button', { name: 'Apply' }).click()
}

/** Add a step to the open Startup or Shutdown tab. */
export async function addStep(page: Page, device: string, command: string) {
  await page.getByRole('button', { name: 'Add step' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByLabel('Device').selectOption({ label: device })
  await dialog.getByLabel('Command', { exact: true }).selectOption({ label: command })
  await dialog.getByRole('button', { name: 'Add step' }).click()
  await expect(dialog).toBeHidden()
}

/** The device id in a device page's URL (`#/devices/<id>`). */
export const deviceIdFromUrl = (page: Page) => new URL(page.url()).hash.split('/').pop()!

/** A sequence as "device command" lines (a wait has no device: " wait"). */
export const stepIds = (steps: { device_id: string | null; command_id: string }[]) => steps.map((s) => `${s.device_id ?? ''} ${s.command_id}`)

/** The activity id in the editor's URL (`#/activities/<id>`), after the wizard creates one or a duplicate. */
export async function activityIdFromUrl(page: Page): Promise<string> {
  await expect(page).toHaveURL(/#\/activities\/act_/)
  return page.url().split('/').pop()!
}
