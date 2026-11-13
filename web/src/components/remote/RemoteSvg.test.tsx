// @vitest-environment jsdom
import { fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { RemoteSvg } from './RemoteSvg'

const key = (c: HTMLElement, id: string) => c.querySelector(`[data-key="${id}"]`)!
const border = (c: HTMLElement, id: string) => key(c, id).querySelectorAll('path, rect, circle, ellipse')

describe('RemoteSvg keyboard', () => {
  it('renders 48 labelled, keyboard-reachable buttons with one tab stop', () => {
    render(<RemoteSvg onSelect={() => {}} selected="select" bound={{ volume_up: ['down'], info: ['tap', 'hold'] }} />)
    const keys = screen.getAllByRole('button')
    expect(keys).toHaveLength(48)
    expect(keys.filter((k) => k.getAttribute('tabindex') === '0')).toHaveLength(1)
    expect(screen.getByRole('button', { name: /^Info, bound: press, long press/ })).toBeTruthy()
    expect(screen.getByRole('button', { name: /^OK, unbound/ }).getAttribute('aria-pressed')).toBe('true')
  })

  it('moves focus with arrow keys and selects with Enter', () => {
    const onSelect = vi.fn()
    render(<RemoteSvg onSelect={onSelect} selected="select" />)
    const ok = screen.getByRole('button', { name: /^OK/ })
    ok.focus()
    fireEvent.keyDown(ok, { key: 'ArrowUp' })
    const up = screen.getByRole('button', { name: /^Up,/ })
    expect(document.activeElement).toBe(up)
    fireEvent.keyDown(up, { key: 'Enter' })
    expect(onSelect).toHaveBeenCalledWith('up')
  })
})

describe('RemoteSvg marks', () => {
  it('draws an inherited key like a bound one, in the global colour, with its dots', () => {
    const text = 'Press → Hubbo → Switch activity · Long press → Off'
    const { container } = render(<RemoteSvg onSelect={() => {}} bound={{ up: ['tap'] }} inherited={{ tv: { kinds: ['tap', 'hold'], text } }} />)
    expect(key(container, 'up').getAttribute('aria-label')).toMatch(/, bound: press$/)
    expect(key(container, 'tv').getAttribute('aria-label')).toMatch(/, global: press, long press$/)
    expect(key(container, 'tv').querySelector('title')?.textContent).toBe(`TV, Global: ${text}`) // keeps the key's name
    expect(key(container, 'select').getAttribute('aria-label')).toMatch(/, unbound$/)
    const strokes = (id: string) => [...border(container, id)].map((e) => e.getAttribute('stroke'))
    expect(strokes('up')).toContain('var(--remote-bound)')
    expect(strokes('tv')).toContain('var(--remote-global)')
    expect(key(container, 'tv').querySelectorAll('circle[fill^="var(--ev-"]')).toHaveLength(2)
  })

  it('colours a global set’s own keys amber, and its own binding wins over an inherited one', () => {
    const { container } = render(<RemoteSvg onSelect={() => {}} source="global" bound={{ tv: ['tap'] }} inherited={{ tv: { kinds: ['hold'], text: 'x' } }} />)
    expect(key(container, 'tv').getAttribute('aria-label')).toMatch(/, bound: press$/)
    expect([...border(container, 'tv')].map((e) => e.getAttribute('stroke'))).toContain('var(--remote-global)')
  })
})
