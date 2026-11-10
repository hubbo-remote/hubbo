// axe on the main pages (WCAG 2 A/AA rules): serious and critical violations fail the test.
import AxeBuilder from '@axe-core/playwright'
import type { Page } from '@playwright/test'
import { expect, open, test } from './hub.ts'

async function expectNoSeriousViolations(page: Page, what: string) {
  const r = await new AxeBuilder({ page }).withTags(['wcag2a', 'wcag2aa', 'wcag21a', 'wcag21aa']).analyze()
  const bad = r.violations.filter((v) => v.impact === 'serious' || v.impact === 'critical')
  const summary = bad.map((v) => `${v.impact} ${v.id}: ${v.help}\n${v.nodes.slice(0, 5).map((n) => `    ${n.target.join(' ')}  ${n.failureSummary?.split('\n')[1] ?? ''}`).join('\n')}`)
  expect(summary, `${what}: axe`).toEqual([])
}

const PAGES: [string, string][] = [
  ['/', 'Dashboard'],
  ['/activities', 'Activities'],
  ['/activities/act_watch_apple_tv', 'Bound buttons'],
  ['/activities/global', 'Global buttons'],
  ['/devices', 'Devices'],
  ['/devices/dev_denon', 'Family Room Denon'],
  ['/integrations', 'Integrations'],
  ['/settings/account', 'Settings'],
  ['/settings/remote', 'Settings'],
  ['/settings/backup', 'Settings'],
  ['/settings/diagnostics', 'Settings'],
]

for (const [hash, what] of PAGES) {
  test(`axe: ${hash}, light and dark`, async ({ page }) => {
    await open(page, hash)
    await expect(page.getByRole('main')).toContainText(what)
    for (const colorScheme of ['light', 'dark'] as const) {
      await page.emulateMedia({ colorScheme })
      await expect.poll(() => page.evaluate(() => document.documentElement.classList.contains('dark'))).toBe(colorScheme === 'dark')
      // Let the colour transitions finish, or axe measures contrast mid-fade.
      await page.waitForFunction(() => document.getAnimations().every((a) => !(a instanceof CSSTransition) || a.playState !== 'running'))
      await expectNoSeriousViolations(page, `${hash} (${colorScheme})`)
    }
  })
}

test('axe: the activity editor with a button selected, and a dialog open', async ({ page }) => {
  await open(page, '/activities/act_watch_apple_tv')
  await page.locator('[data-key="up"]').click()
  await expectNoSeriousViolations(page, 'editor, button selected')
  await page.getByRole('tab', { name: /Startup/ }).click()
  await page.getByRole('button', { name: 'Add step' }).click()
  await expect(page.getByRole('dialog')).toBeVisible()
  await expectNoSeriousViolations(page, 'add step dialog')
})

test('the skip link moves focus to the content without changing the route', async ({ page }) => {
  await open(page, '/devices')
  await page.keyboard.press('Tab')
  const skip = page.getByRole('link', { name: 'Skip to content' })
  await expect(skip).toBeFocused()
  await page.keyboard.press('Enter')
  await expect(page).toHaveURL(/#\/devices$/)
  await expect(page.getByRole('main')).toBeFocused()
  await expect(page.getByRole('heading', { name: 'Devices', level: 1 })).toBeVisible()
})

test('the remote picture’s buttons are big enough to tap (at least 24×24 px, WCAG 2.5.8)', async ({ page, isMobile }) => {
  test.skip(!isMobile, 'tap targets: phone only')
  await open(page, '/activities/act_watch_apple_tv')
  await page.locator('[data-key="up"]').waitFor()
  const boxes = await page.locator('[data-key]').evaluateAll((els) => els.map((e) => ({ id: e.getAttribute('data-key')!, r: e.getBoundingClientRect().toJSON() as DOMRect })))
  expect(boxes.filter(({ r }) => r.width < 24 || r.height < 24).map(({ id, r }) => `${id} ${Math.round(r.width)}×${Math.round(r.height)}`)).toEqual([])
  // A tap anywhere in a key's minimum hit area lands on that key (or on nothing), never on a neighbour.
  const wrong = await page.locator('[data-key] > rect').evaluateAll((rects) =>
    rects.flatMap((rect) => {
      const id = rect.parentElement!.getAttribute('data-key')
      const r = rect.getBoundingClientRect()
      const pts = [[r.left + r.width / 2, r.top + r.height / 2], [r.left + 1, r.top + 1], [r.right - 1, r.top + 1], [r.left + 1, r.bottom - 1], [r.right - 1, r.bottom - 1]]
      return pts.map(([x, y]) => document.elementFromPoint(x!, y!)?.closest('[data-key]')?.getAttribute('data-key')).filter((hit) => hit && hit !== id).map((hit) => `${id} → ${hit}`)
    }),
  )
  expect(wrong).toEqual([])
})
