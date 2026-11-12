// @vitest-environment jsdom
import { fireEvent, render, screen } from '@testing-library/react'
import { Moon, Pencil, Sun } from 'lucide-react'
import { useState } from 'react'
import { describe, expect, it, vi } from 'vitest'
import { Menu, MenuItem, MenuSeparator } from './menu'

function Harness({ onEdit = () => {} }: { onEdit?: () => void }) {
  const [open, setOpen] = useState(false)
  return (
    <div>
      <p>outside</p>
      <Menu label="Things" open={open} onOpenChange={setOpen} trigger={(p) => <button {...p}>More</button>}>
        <MenuItem icon={Pencil} onClick={onEdit}>
          Edit
        </MenuItem>
        <MenuSeparator />
        <MenuItem icon={Sun} checked>
          Light
        </MenuItem>
        <MenuItem icon={Moon} checked={false}>
          Dark
        </MenuItem>
      </Menu>
    </div>
  )
}

const more = () => screen.getByRole('button', { name: 'More' })

describe('Menu', () => {
  it('gives its trigger the menu props and toggles from it', () => {
    render(<Harness />)
    expect(more().getAttribute('aria-haspopup')).toBe('menu')
    fireEvent.click(more())
    expect(more().getAttribute('aria-expanded')).toBe('true')
    expect(screen.getByRole('menu', { name: 'Things' })).toBeTruthy()
    fireEvent.click(more())
    expect(screen.queryByRole('menu')).toBeNull()
  })

  it('closes on a press outside, on Esc (focus back on the trigger), and once an item is chosen', () => {
    const onEdit = vi.fn()
    render(<Harness onEdit={onEdit} />)
    fireEvent.click(more())
    fireEvent.pointerDown(screen.getByText('outside'))
    expect(screen.queryByRole('menu')).toBeNull()

    fireEvent.click(more())
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' })
    expect(screen.queryByRole('menu')).toBeNull()
    expect(document.activeElement).toBe(more())

    fireEvent.click(more())
    fireEvent.click(screen.getByRole('menuitem', { name: 'Edit' }))
    expect(onEdit).toHaveBeenCalled()
    expect(screen.queryByRole('menu')).toBeNull()
  })

  it('shows radio items checked or not, and arrows move across both kinds', () => {
    render(<Harness />)
    fireEvent.click(more())
    expect(screen.getByRole('menuitemradio', { name: 'Light' }).getAttribute('aria-checked')).toBe('true')
    expect(screen.getByRole('menuitemradio', { name: 'Dark' }).getAttribute('aria-checked')).toBe('false')
    expect(document.activeElement).toBe(screen.getByRole('menuitemradio', { name: 'Light' })) // the checked item takes focus
    screen.getByRole('menuitem', { name: 'Edit' }).focus()
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'ArrowDown' })
    expect(document.activeElement).toBe(screen.getByRole('menuitemradio', { name: 'Light' }))
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'End' })
    expect(document.activeElement).toBe(screen.getByRole('menuitemradio', { name: 'Dark' }))
  })

  it('focuses the first item on open when none is checked', () => {
    render(
      <Menu label="Plain" open onOpenChange={() => {}} trigger={(p) => <button {...p}>More</button>}>
        <MenuItem icon={Pencil}>Edit</MenuItem>
        <MenuItem icon={Sun}>Other</MenuItem>
      </Menu>,
    )
    expect(document.activeElement).toBe(screen.getByRole('menuitem', { name: 'Edit' }))
  })
})
