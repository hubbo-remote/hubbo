import type { Page } from '@playwright/test'
import { advance, api, control, expect, loginHeading, open, session, signIn, SIM_URL, test } from './hub.ts'
import { releaseNotesUrl } from '../src/lib/firmware.ts'

// Online updates from Status → Firmware (docs/design/firmware-updates.md), against the fake release feed
// (/__mock/release, /sim/release). Both targets run on hub time: a check, the download, the write and the
// reboot only move with /advance. hub-sim runs a development build, which is never offered anything, so each
// test sets a release version first.

const checkNow = (page: Page) => page.getByRole('button', { name: 'Check now' })

test.beforeEach(async ({ page }) => {
  await control(page, 'release?running=1.3.0&version=1.4.0&fault=none')
})

test('check, see the update, install it: download, write, restart, then the new version', async ({ page }) => {
  const signInAfter = !SIM_URL || (await session(page)).auth_mode === 'enforced'
  await open(page, '/status/firmware')
  await expect(page.getByText('Hubbo 1.3.0', { exact: true })).toBeVisible()

  await checkNow(page).click()
  await advance(page, 1_000)
  const block = page.getByText('Hubbo 1.4.0 is available')
  await expect(block).toBeVisible()
  const notes = page.getByRole('link', { name: 'What’s new' })
  await expect(notes).toHaveAttribute('href', releaseNotesUrl('1.4.0'))
  await expect(notes).toHaveAttribute('target', '_blank')

  // Install asks first; Cancel changes nothing.
  await page.getByRole('button', { name: 'Install', exact: true }).click()
  const confirm = page.getByRole('dialog', { name: 'Update to Hubbo 1.4.0?' })
  await expect(confirm).toContainText('Your remote won’t work for about a minute')
  await confirm.getByRole('button', { name: 'Cancel' }).click()
  expect((await api(page).get('/admin/firmware')).update.state).toBe('idle')
  await page.getByRole('button', { name: 'Install', exact: true }).click()
  await confirm.getByRole('button', { name: 'Update' }).click()

  await expect(page.getByText('Updating to Hubbo 1.4.0')).toBeVisible()
  await expect(page.getByRole('progressbar', { name: 'Download progress' })).toBeVisible()
  await expect(checkNow(page)).toBeDisabled()
  await advance(page, 1_000) // the download
  await expect(page.getByRole('progressbar', { name: 'Write progress' })).toBeVisible()
  await advance(page, 3_000) // the write, then the restart (which ends every session)

  if (signInAfter) {
    await expect(loginHeading(page)).toBeVisible()
    await signIn(page)
  }
  await expect(page.getByRole('status').filter({ hasText: 'Hubbo is now running 1.4.0.' })).toBeVisible()
  await expect(page.getByText('Hubbo 1.4.0', { exact: true })).toBeVisible()
  await expect(page.getByText('Hubbo 1.4.0 is available')).toHaveCount(0)
})

test('Check now spins while it checks; a failed check says why and keeps the update it knew', async ({ page }) => {
  await open(page, '/status/firmware')
  await checkNow(page).click()
  await expect(checkNow(page)).toBeDisabled()
  await expect(checkNow(page).locator('svg')).toHaveClass(/animate-spin/)
  await advance(page, 1_000)
  await expect(checkNow(page)).toBeEnabled()
  await expect(page.getByText('Hubbo 1.4.0 is available')).toBeVisible()

  await control(page, 'release?fault=unreachable')
  await checkNow(page).click()
  await advance(page, 1_000)
  await expect(page.getByText('Hubbo couldn’t reach the internet to check for updates.')).toBeVisible()
  await expect(page.getByText('Hubbo 1.4.0 is available')).toBeVisible()

  await control(page, 'release?fault=bad_signature')
  await checkNow(page).click()
  await advance(page, 1_000)
  await expect(page.getByText('couldn’t confirm it’s genuine')).toBeVisible()

  // Nothing newer: up to date.
  await control(page, 'release?fault=none&version=1.2.0')
  await checkNow(page).click()
  await advance(page, 1_000)
  await expect(page.getByText('Hubbo is up to date. Last checked')).toBeVisible()
  await expect(page.getByText(/is available/)).toHaveCount(0)
})

test('after a rollback, the card says the update didn’t start', async ({ page }) => {
  await control(page, 'release?running=1.4.0&rolled_back_from=1.5.0')
  await open(page, '/status/firmware')
  await expect(page.getByRole('status').filter({ hasText: 'The update didn’t start' })).toContainText('The update to 1.5.0 didn’t start, so Hubbo went back to 1.4.0.')
})

test('the daily check can be turned off, and stays off', async ({ page }) => {
  await open(page, '/status/firmware')
  const daily = page.getByRole('switch', { name: 'Check for updates daily' })
  await expect(daily).toHaveAttribute('aria-checked', 'true')
  await daily.click()
  await expect(daily).toHaveAttribute('aria-checked', 'false')
  await expect.poll(async () => (await api(page).get('/admin/hub')).check_updates).toBe(false)
  await page.reload()
  await expect(page.getByRole('switch', { name: 'Check for updates daily' })).toHaveAttribute('aria-checked', 'false')
})
