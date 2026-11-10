// Activity editor steps shared by the specs (selectors in one place).
import type { Page } from '@playwright/test'
import { buttonLabel } from '../src/lib/buttons.ts'
import { expect } from './hub.ts'

/** Select a button on the remote picture and bind one of its events. */
export async function bind(page: Page, button: string, event: string, device: string, command: string, key?: string) {
  await page.locator(`[data-key="${button}"]`).click()
  await page.getByRole('button', { name: `Bind ${buttonLabel(button)} ${event}` }).click()
  await page.getByLabel('Device').selectOption({ label: device })
  await page.getByLabel('Command').selectOption({ label: command })
  if (key) await page.getByLabel('Key', { exact: true }).selectOption(key)
  await page.getByRole('button', { name: 'Apply' }).click()
}

/** Add a step to the open Startup or Shutdown tab. */
export async function addStep(page: Page, device: string, command: string) {
  await page.getByRole('button', { name: 'Add step' }).click()
  const dialog = page.getByRole('dialog')
  await dialog.getByLabel('Device').selectOption({ label: device })
  await dialog.getByLabel('Command').selectOption({ label: command })
  await dialog.getByRole('button', { name: 'Add step' }).click()
  await expect(dialog).toBeHidden()
}

/** The activity id in the editor's URL (`#/activities/<id>`), after a create or duplicate. */
export async function activityIdFromUrl(page: Page): Promise<string> {
  await expect(page).toHaveURL(/#\/activities\/act_/)
  return page.url().split('/').pop()!
}
