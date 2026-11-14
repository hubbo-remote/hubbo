import { ensureAdmin, expect, expectNotStoredInBrowser, open, test } from './hub.ts'

test('API token is shown exactly once', async ({ page }) => {
  await ensureAdmin(page)
  await open(page, '/settings/tokens')
  await page.getByLabel('Name').fill('E2E agent')
  await page.getByRole('button', { name: 'Create token' }).click()

  const dialog = page.getByRole('dialog')
  await expect(dialog.getByRole('heading', { name: 'Copy your token' })).toBeVisible()
  const token = (await dialog.locator('code').first().textContent())!.trim()
  expect(token).toMatch(/^hbt_[a-z0-9]{8}_[A-Za-z0-9_-]{43}$/)
  await dialog.getByRole('button', { name: 'Done' }).click()

  await expect(page.getByText('E2E agent')).toBeVisible()
  await expectNotStoredInBrowser(page, token)
  await page.reload()
  await expect(page.getByText('E2E agent')).toBeVisible()
  await expectNotStoredInBrowser(page, token)

  await page.getByRole('button', { name: 'Revoke E2E agent' }).click()
  await expect(page.getByText('E2E agent')).toBeHidden()
})

test('Copy works over plain HTTP (no clipboard API), and says so only when it did', async ({ page }) => {
  // The hub is plain HTTP: no secure context, so no navigator.clipboard. Record what execCommand copies.
  await page.addInitScript(() => {
    const w = window as unknown as { copies: { text: string; ok: boolean }[]; failCopy?: boolean }
    w.copies = []
    Object.defineProperty(window, 'isSecureContext', { value: false })
    const exec = document.execCommand.bind(document)
    document.execCommand = (cmd: string, ...rest: [boolean?, string?]) => {
      if (cmd !== 'copy') return exec(cmd, ...rest)
      const ok = !w.failCopy && exec(cmd, ...rest)
      w.copies.push({ text: (document.activeElement as HTMLTextAreaElement | null)?.value ?? '', ok })
      return ok
    }
  })
  await ensureAdmin(page)
  await open(page, '/settings/tokens')
  await page.getByLabel('Name').fill('E2E copy')
  await page.getByRole('button', { name: 'Create token' }).click()
  const dialog = page.getByRole('dialog')
  const token = (await dialog.locator('code').first().textContent())!.trim()
  const copies = () => page.evaluate(() => (window as unknown as { copies: unknown[] }).copies)

  const copy = dialog.getByRole('button', { name: 'Copy' })
  await copy.click()
  await expect(dialog.getByRole('button', { name: 'Copied' })).toBeFocused() // the dialog keeps focus
  expect(await copies()).toEqual([{ text: token, ok: true }])

  // Both ways fail: no "Copied", and the token is selected to copy by hand.
  await page.evaluate(() => ((window as unknown as { failCopy: boolean }).failCopy = true))
  await dialog.getByRole('button', { name: 'Copied' }).click()
  await expect(dialog.getByText('Couldn’t copy it.')).toBeVisible()
  await expect(dialog.getByRole('button', { name: 'Copy', exact: true })).toBeVisible()
  expect(await page.evaluate(() => window.getSelection()?.toString().trim())).toBe(token)

  await dialog.getByRole('button', { name: 'Done' }).click()
  await expectNotStoredInBrowser(page, token)
  await page.getByRole('button', { name: 'Revoke E2E copy' }).click()
})
