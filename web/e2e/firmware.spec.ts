import type { Page } from '@playwright/test'
import type { FirmwareStatus } from '../src/api/types.ts'
import { buildFirmwareImage } from '../mock/firmware.ts'
import { advance, api, chooseFirmware, control, expect, loginHeading, open, session, signIn, SIM_URL, test } from './hub.ts'

// Firmware updates from Status → Firmware (design phase5 §2). Both targets run on hub time: a write takes
// 2 s and the reboot 1 s of /advance. The reboot ends every session, like the board's: the mock
// always asks for a sign-in afterwards, hub-sim under --auth (no-auth hub-sim has no sessions).
// hub-sim reports the same version after an update on purpose (only a board's image changes;
// the board gate checks the real version), so there the UI says the update didn't take.

const firmware = (page: Page): Promise<FirmwareStatus> => api(page).get('/admin/firmware')

test('install an update: write progress, the restart, sign in again, then the new version', async ({ page }) => {
  const signInAfter = !SIM_URL || (await session(page)).auth_mode === 'enforced'
  const { board, version: before } = await firmware(page)
  await open(page, '/status/firmware')
  await expect(page.getByText(before, { exact: true })).toBeVisible()
  await expect(page.getByText('remote may lag')).toBeVisible()

  await chooseFirmware(page, await buildFirmwareImage({ board, version: 'v9.9.9-e2e' }))
  await expect(page.getByText(`Hubbo v9.9.9-e2e for ${board}`)).toBeVisible()
  await page.getByRole('button', { name: 'Install update' }).click()
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await expect(page.getByRole('button', { name: 'Install update' })).toBeDisabled()

  await advance(page, 2_000)
  // hub-sim's 1 s reboot can pass between two polls; the mock holds it until the next advance.
  if (!SIM_URL) await expect(page.getByText('Restarting…')).toBeVisible()
  await advance(page, 1_000)

  // The session is gone (the poll got a 401): sign in again, and the section says what's running.
  if (signInAfter) {
    await expect(loginHeading(page)).toBeVisible()
    await signIn(page)
  }
  const installed = page.getByRole('status').filter({ hasText: 'Update installed' })
  const notTaken = page.getByRole('alert').filter({ hasText: 'The update didn’t take' })
  // hub-sim's version stays put (see the top): the honest answer there is "didn't take".
  if (SIM_URL) await expect(notTaken).toContainText(`Still running ${before}, not v9.9.9-e2e`)
  else await expect(installed).toContainText(`Now running v9.9.9-e2e (was ${before})`)
  expect((await firmware(page)).update.state).toBe('idle')
})

test('a restart back into the old version says the update didn’t take', async ({ page }) => {
  test.skip(!!SIM_URL, 'the mock can boot the old image again (/__mock/firmware?revert=1)')
  const { board, version } = await firmware(page)
  await control(page, 'firmware?revert=1')
  await open(page, '/status/firmware')
  await chooseFirmware(page, await buildFirmwareImage({ board, version: 'v9.9.9-e2e' }))
  await page.getByRole('button', { name: 'Install update' }).click()
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await advance(page, 3_000)
  await expect(loginHeading(page)).toBeVisible()
  await signIn(page)
  await expect(page.getByRole('alert').filter({ hasText: 'The update didn’t take' })).toContainText(`Still running ${version}, not v9.9.9-e2e`)
  await expect(page.getByRole('status').filter({ hasText: 'Update installed' })).toHaveCount(0)
})

test('a wrong file is refused before anything is written', async ({ page }) => {
  await open(page, '/status/firmware')
  const refused = page.getByRole('alert').filter({ hasText: 'This file wasn’t installed' })

  await chooseFirmware(page, await buildFirmwareImage({ board: 'some_other_board' }))
  await page.getByRole('button', { name: 'Install update' }).click()
  await expect(refused).toContainText('This update is for a different device')
  await expect(refused).toContainText('Nothing changed.')

  await chooseFirmware(page, 'just text, nothing like a firmware image, but long enough to get past the size check')
  await expect(page.getByText('This doesn’t look like a Hubbo update.')).toBeVisible()
  await expect(refused).toHaveCount(0) // a new file clears the old refusal
  await page.getByRole('button', { name: 'Install update' }).click()
  await expect(refused).toContainText("This isn't a Hubbo update file")
  expect((await firmware(page)).update.state).toBe('idle')
})

test('a failed write says the running version stays', async ({ page }) => {
  const { board, version } = await firmware(page)
  await control(page, 'firmware?fail=1')
  await open(page, '/status/firmware')
  await chooseFirmware(page, await buildFirmwareImage({ board }))
  await page.getByRole('button', { name: 'Install update' }).click()
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await advance(page, 2_000)
  const failed = page.getByRole('alert').filter({ hasText: 'The update failed' })
  await expect(failed).toContainText(`Still running ${version}. Try again.`)
  await expect(page.getByRole('button', { name: 'Install update' })).toBeEnabled()
})
