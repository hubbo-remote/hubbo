import type { Page } from '@playwright/test'
import type { FirmwareStatus } from '../src/api/types.ts'
import { buildFirmwareImage } from '../mock/firmware.ts'
import { advance, api, chooseFirmware, control, expect, loginHeading, open, session, signIn, SIM_URL, test } from './hub.ts'

// Firmware updates from Status → Firmware (design phase5 §2). Both targets run on hub time: a write takes
// 2 s and the reboot 1 s of /advance. The reboot ends every session, like the board's: the mock
// always asks for a sign-in afterwards, hub-sim under --auth (no-auth hub-sim has no sessions).

const firmware = (page: Page): Promise<FirmwareStatus> => api(page).get('/admin/firmware')
/** The file's Update button ("Update from a file"). */
const update = (page: Page) => page.getByRole('button', { name: 'Update', exact: true })

test('install an update: write progress, the restart, sign in again, then the new version', async ({ page }) => {
  const signInAfter = !SIM_URL || (await session(page)).auth_mode === 'enforced'
  const { board, version: before } = await firmware(page)
  await open(page, '/status/firmware')
  await expect(page.getByText(`Hubbo ${before}`, { exact: true })).toBeVisible()

  await chooseFirmware(page, await buildFirmwareImage({ board, version: 'v9.9.9-e2e' }))
  await expect(page.getByText(`Hubbo v9.9.9-e2e for ${board}`)).toBeVisible()
  await update(page).click()
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await expect(update(page)).toBeDisabled()

  await advance(page, 2_000)
  // hub-sim's 1 s reboot can pass between two polls; the mock holds it until the next advance.
  if (!SIM_URL) await expect(page.getByText('Restarting Hubbo…')).toBeVisible()
  await advance(page, 1_000)

  // The session is gone (the poll got a 401): sign in again, and the section says what's running.
  if (signInAfter) {
    await expect(loginHeading(page)).toBeVisible()
    await signIn(page)
  }
  await expect(page.getByRole('status').filter({ hasText: 'Hubbo is now running v9.9.9-e2e.' })).toBeVisible()
  expect((await firmware(page)).update.state).toBe('idle')
})

test('a restart back into the old version says the update didn’t start', async ({ page }) => {
  test.skip(!!SIM_URL, 'the mock can boot the old image again (/__mock/firmware?revert=1)')
  const { board, version } = await firmware(page)
  await control(page, 'firmware?revert=1')
  await open(page, '/status/firmware')
  await chooseFirmware(page, await buildFirmwareImage({ board, version: 'v9.9.9-e2e' }))
  await update(page).click()
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await advance(page, 3_000)
  await expect(loginHeading(page)).toBeVisible()
  await signIn(page)
  await expect(page.getByRole('status').filter({ hasText: 'The update didn’t start' })).toContainText(`The update to v9.9.9-e2e didn’t start, so Hubbo went back to ${version}.`)
  await expect(page.getByRole('status').filter({ hasText: 'Hubbo is now running' })).toHaveCount(0)
})

test('a wrong file is refused before anything is written', async ({ page }) => {
  await open(page, '/status/firmware')
  const refused = page.getByRole('alert').filter({ hasText: 'Hubbo didn’t install this file' })

  await chooseFirmware(page, await buildFirmwareImage({ board: 'some_other_board' }))
  await update(page).click()
  await expect(refused).toContainText('This update is for a different device')
  await expect(refused).toContainText('Nothing on Hubbo has changed.')

  await chooseFirmware(page, 'just text, nothing like a firmware image, but long enough to get past the size check')
  await expect(page.getByText('This doesn’t look like a Hubbo update.')).toBeVisible()
  await expect(refused).toHaveCount(0) // a new file clears the old refusal
  await update(page).click()
  await expect(refused).toContainText("This isn't a Hubbo update file")
  expect((await firmware(page)).update.state).toBe('idle')
})

test('a failed write says the running version stays', async ({ page }) => {
  const { board, version } = await firmware(page)
  await control(page, 'firmware?fail=1')
  await open(page, '/status/firmware')
  await chooseFirmware(page, await buildFirmwareImage({ board }))
  await update(page).click()
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await advance(page, 2_000)
  const failed = page.getByRole('alert').filter({ hasText: 'The update didn’t install' })
  await expect(failed).toContainText(`Hubbo is still running ${version}, so nothing has changed.`)
  await expect(update(page)).toBeEnabled()
})
