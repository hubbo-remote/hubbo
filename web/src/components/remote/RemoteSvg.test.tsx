// @vitest-environment jsdom
import { fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import { RemoteSvg } from './RemoteSvg'

describe('RemoteSvg', () => {
  it('renders 48 labelled, keyboard-reachable buttons with one tab stop', () => {
    render(<RemoteSvg onSelect={() => {}} selected="select" bound={{ volume_up: ['down'], info: ['tap', 'hold'] }} />)
    const keys = screen.getAllByRole('button')
    expect(keys).toHaveLength(48)
    expect(keys.filter((k) => k.getAttribute('tabindex') === '0')).toHaveLength(1)
    expect(screen.getByRole('button', { name: /^Info, bound: tap, hold/ })).toBeTruthy()
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
