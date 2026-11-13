// The Remote section (Global or one activity's buttons), the nav around it, and the phone's sheet
// and gear. Desktop tests run in `app`; the phone ones in `phone` (390 px, touch).
import { bind } from './editor.ts'
import { api, expect, open, test } from './hub.ts'

const ACTIVITY = 'act_watch_apple_tv'

test.describe('desktop', () => {
  test.skip(({ isMobile }) => isMobile, 'desktop layout')

  test('the picker switches between Global and an activity in place, and Back returns', async ({ page }) => {
    await open(page, '/remote')
    const nav = page.getByRole('navigation', { name: 'Main' }).first()
    await expect(nav.getByRole('link', { name: 'Remote' })).toHaveAttribute('aria-current', 'page')
    const picker = page.getByLabel('Buttons for')
    await expect(picker.locator('option:checked')).toHaveText('Global')
    await expect(page.getByText('This activity')).toHaveCount(0) // no source legend on Global

    // A marker on the page's heading survives the switch only if the page isn't remounted.
    await page.getByRole('heading', { name: 'Remote' }).evaluate((h) => h.setAttribute('data-mark', '1'))
    await picker.focus()
    await picker.selectOption(ACTIVITY)
    await expect(page).toHaveURL(new RegExp(`#/remote/${ACTIVITY}$`))
    await expect(page.getByText('This activity')).toBeVisible()
    await expect(page.locator('[data-key="tv"]')).toHaveAttribute('aria-label', /global: press$/) // inherited, amber
    await expect(page.getByRole('heading', { name: 'Remote' })).toHaveAttribute('data-mark', '1') // the same page
    await expect(picker).toBeFocused()

    await page.goBack()
    await expect(page).toHaveURL(/#\/remote$/)
    await expect(picker.locator('option:checked')).toHaveText('Global')
  })

  test('an activity’s buttons save from the Remote page', async ({ page }) => {
    await open(page, `/remote/${ACTIVITY}`)
    await bind(page, 'number_5', 'press', 'Family Room Apple TV', 'Keyboard key…', 'ARROW_UP')
    await expect(page.getByText('Unsaved changes')).toBeVisible()
    await page.getByRole('button', { name: /^Save/ }).click()
    await expect(page.getByText('Saved', { exact: true })).toBeVisible()
    const a = await api(page).get(`/activities/${ACTIVITY}`)
    expect(a.bindings['number_5:down'][0]).toMatchObject({ command_id: 'key_down', params: { key: 'ARROW_UP' } })
  })

  test('switching sets with unsaved changes asks first', async ({ page }) => {
    await open(page, `/remote/${ACTIVITY}`)
    await bind(page, 'number_5', 'press', 'Family Room Apple TV', 'Keyboard key…', 'ARROW_UP')
    const picker = page.getByLabel('Buttons for')
    page.once('dialog', (d) => (expect(d.message()).toBe('Discard unsaved changes?'), d.dismiss()))
    await picker.selectOption('')
    await expect(page).toHaveURL(new RegExp(`#/remote/${ACTIVITY}$`))
    await expect(page.getByText('Unsaved changes')).toBeVisible()
    await expect(picker.locator('option:checked')).not.toHaveText('Global')

    page.once('dialog', (d) => d.accept())
    await picker.selectOption('')
    await expect(page).toHaveURL(/#\/remote$/)
    await expect(page.getByText('Unsaved changes')).toHaveCount(0)
  })

  test('saving the globals over someone else’s save shows the conflict', async ({ page }) => {
    await open(page, '/remote')
    await bind(page, 'number_5', 'press', 'Family Room Apple TV', 'Keyboard key…', 'ARROW_UP')
    const hub = api(page)
    const theirs = await hub.get('/global-bindings')
    expect((await hub.put('/global-bindings', theirs)).status()).toBe(200)

    await page.getByRole('button', { name: /^Save/ }).click()
    const dialog = page.getByRole('dialog')
    await expect(dialog.getByRole('heading', { name: 'Changed somewhere else' })).toBeVisible()
    await expect(dialog).toContainText('Buttons: 1 button differs')
    await dialog.getByRole('button', { name: 'Save mine' }).click()
    await expect(dialog).toBeHidden()
    expect((await hub.get('/global-bindings')).bindings['number_5:down']).toBeTruthy()
  })

  test('the nav shows what’s on, quietly; Settings and the theme toggle stay in the sidebar', async ({ page }) => {
    await open(page, '/settings/account')
    const side = page.locator('aside')
    const active = await api(page).activeActivity()
    const name = active ? (await api(page).get(`/activities/${active}`)).name : 'Off'
    await expect(side.getByRole('status')).toHaveText(name)
    await expect(side.getByRole('status').getByRole('link')).toHaveCount(0)
    await expect(side.getByRole('link', { name: 'Settings' })).toHaveAttribute('aria-current', 'page')
    await expect(side.getByRole('link', { name: 'Activities' })).not.toHaveAttribute('aria-current', 'page')
    await expect(side.getByRole('radiogroup', { name: 'Theme' })).toBeVisible()
    await expect(page.getByRole('button', { name: 'Settings and theme' })).toBeHidden() // the gear is the phone's
  })

  test('the tile menu closes on an outside click and on choosing an item; Delete is set apart', async ({ page }) => {
    await open(page, '/')
    const more = page.getByRole('button', { name: 'More for Watch Apple TV' })
    await more.click()
    const menu = page.getByRole('menu')
    await expect(menu.getByRole('separator')).toHaveCount(1)
    await page.getByRole('heading', { name: 'Activities' }).click()
    await expect(menu).toBeHidden()
    await more.click()
    await menu.getByRole('menuitem', { name: 'Edit' }).click()
    await expect(page).toHaveURL(/#\/activities\/act_watch_apple_tv$/)
  })
})

test.describe('phone', () => {
  test.skip(({ isMobile }) => !isMobile, 'phone layout')

  test('the tab bar has five sections, and the gear holds the theme and Settings', async ({ page }) => {
    await open(page, '/')
    const tabs = page.getByRole('navigation', { name: 'Main' }).last()
    await expect(tabs.getByRole('link')).toHaveText(['Activities', 'Devices', 'Remote', 'Integrations', 'Status'])
    await page.getByRole('button', { name: 'Settings and theme' }).click()
    const menu = page.getByRole('menu')
    await menu.getByRole('menuitemradio', { name: 'Dark' }).click()
    await expect(page.locator('html')).toHaveClass(/dark/)
    await page.getByRole('button', { name: 'Settings and theme' }).click()
    await expect(menu.getByRole('menuitemradio', { name: 'Dark' })).toHaveAttribute('aria-checked', 'true')
    await menu.getByRole('menuitem', { name: 'Settings' }).click()
    await expect(page).toHaveURL(/#\/settings/)
  })

  test('a tapped key opens its editor in a sheet; another key switches it; Done and Esc close it', async ({ page }) => {
    await open(page, '/remote')
    await expect(page.getByRole('dialog')).toHaveCount(0) // nothing open until a tap
    await page.locator('[data-key="tv"]').tap()
    const sheet = page.getByRole('dialog', { name: 'TV button' })
    await expect(sheet).toBeVisible()
    await expect(sheet).toBeFocused()
    await expect(sheet.getByText('Press', { exact: true })).toBeVisible()

    await page.locator('[data-key="movie"]').tap()
    await expect(page.getByRole('dialog', { name: 'Movie button' })).toBeVisible()
    await page.getByRole('button', { name: 'Done' }).click()
    await expect(page.getByRole('dialog')).toHaveCount(0)
    await expect(page.locator('[data-key="movie"]')).toBeFocused()

    await page.locator('[data-key="tv"]').tap()
    await page.keyboard.press('Escape')
    await expect(page.getByRole('dialog')).toHaveCount(0)
  })
})
