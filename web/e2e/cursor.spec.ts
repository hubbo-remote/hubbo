import { expect, open, test } from './hub.ts'

// Everything clickable shows the pointer: a base rule in index.css, since Tailwind's preflight leaves buttons on the arrow.
test('buttons, tabs, switches, remote keys and menu items show the pointer', async ({ page }) => {
  await open(page, '/integrations')
  await expect(page.getByRole('switch', { name: 'Enable Home Assistant' })).toHaveCSS('cursor', 'pointer')
  await expect(page.getByRole('button', { name: 'Save' }).first()).toHaveCSS('cursor', 'pointer')

  await open(page, '/status')
  await expect(page.getByRole('tab').first()).toHaveCSS('cursor', 'pointer')
  await expect(page.getByRole('link').first()).toHaveCSS('cursor', 'pointer')

  await open(page, '/remote')
  await expect(page.locator('[data-key="up"]')).toHaveCSS('cursor', 'pointer')

  await open(page, '/')
  await page.getByRole('list', { name: 'Activities' }).getByRole('button', { name: /^More for / }).first().click()
  await expect(page.getByRole('menuitem', { name: 'Edit' })).toHaveCSS('cursor', 'pointer')
})
