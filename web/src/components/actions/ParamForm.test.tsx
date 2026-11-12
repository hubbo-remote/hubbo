// @vitest-environment jsdom
import { fireEvent, render, screen } from '@testing-library/react'
import { useState } from 'react'
import { describe, expect, it, vi } from 'vitest'
import type { ParamSpec } from '@/api/types'
import { ParamForm } from './ParamForm'

const select = (label: RegExp) => screen.getByLabelText(label) as HTMLSelectElement
const shown = (el: HTMLSelectElement) => el.selectedOptions[0]?.textContent

describe('ParamForm selects show exactly what is stored', () => {
  it('a required activity ref that is unset shows "Choose…", and picking the only activity sets it', () => {
    const onChange = vi.fn()
    const p: ParamSpec = { name: 'activity_id', type: 'activity_ref', required: true }
    render(<ParamForm commandId="test" name="Test" params={[p]} values={{}} onChange={onChange} activities={[{ id: 'act_tv', name: 'Watch TV' }]} />)
    expect(select(/Activity id/).value).toBe('')
    expect(shown(select(/Activity id/))).toBe('Choose…')
    fireEvent.change(select(/Activity id/), { target: { value: 'act_tv' } })
    expect(onChange).toHaveBeenCalledWith({ activity_id: 'act_tv' })
  })

  it('an optional enum holding a value that is no longer an option shows it, and "Not set" clears it', () => {
    const onChange = vi.fn()
    const p: ParamSpec = { name: 'source', type: 'enum', required: false, options: ['HDMI 1', 'HDMI 2'] }
    render(<ParamForm commandId="test" name="Test" params={[p]} values={{ source: 'Phono' }} onChange={onChange} />)
    expect(select(/Source/).value).toBe('Phono')
    expect(shown(select(/Source/))).toBe('Phono (not available)')
    fireEvent.change(select(/Source/), { target: { value: '' } })
    expect(onChange).toHaveBeenCalledWith({})
  })

  it('a stored activity ref is shown as stored while the activities are still loading', () => {
    const p: ParamSpec = { name: 'activity_id', type: 'activity_ref', required: true }
    render(<ParamForm commandId="test" name="Test" params={[p]} values={{ activity_id: 'act_tv' }} onChange={() => {}} activities={[]} />)
    expect(select(/Activity id/).value).toBe('act_tv')
    expect(shown(select(/Activity id/))).toBe('act_tv')
  })
})

// Flame effect on a LIFX light (hub-sim's live Home Assistant): optional params with defaults.
const flame: ParamSpec[] = [
  { name: 'speed', label: 'Speed', type: 'int', required: false, minimum: 1, maximum: 25, unit: 's', default: 3, description: 'How fast the flames move.' },
  { name: 'power_on', label: 'Power on', type: 'bool', required: false, default: true },
]
function Stateful({ params, initial = {}, inline }: { params: ParamSpec[]; initial?: Record<string, number | boolean>; inline?: boolean }) {
  const [values, setValues] = useState<Record<string, number | boolean | string>>(initial)
  return (
    <>
      <ParamForm commandId="lifx.effect_flame" name="Flame effect" params={params} values={values} onChange={setValues} inline={inline} />
      <output data-testid="values">{JSON.stringify(values)}</output>
    </>
  )
}
const toggle = () => screen.getByRole('button', { name: 'More settings for Flame effect' })
const values = () => JSON.parse(screen.getByTestId('values').textContent!)

describe('ParamForm optional params', () => {
  it('keeps them in a drawer whose toggle stays while a value is set, and Reset unsets it', () => {
    render(<Stateful params={flame} inline />)
    expect(screen.queryByLabelText(/Speed/)).toBeNull()
    expect(toggle().getAttribute('aria-expanded')).toBe('false')
    fireEvent.click(toggle())
    const speed = screen.getByLabelText('Speed (s)') as HTMLInputElement
    expect(speed.value).toBe('3') // its default, shown but not sent
    expect(values()).toEqual({})
    fireEvent.change(speed, { target: { value: '7' } })
    expect(values()).toEqual({ speed: 7 })
    fireEvent.click(toggle()) // still there: closing works with a value set
    expect(toggle().getAttribute('aria-expanded')).toBe('false')
    expect(screen.queryByLabelText(/Speed/)).toBeNull()
    fireEvent.click(toggle())
    fireEvent.click(screen.getByRole('button', { name: 'Reset to default' }))
    expect(values()).toEqual({})
  })

  it('opens on a stored action that has settings', () => {
    render(<Stateful params={flame} initial={{ speed: 10 }} />)
    expect(toggle().getAttribute('aria-expanded')).toBe('true')
    expect((screen.getByLabelText('Speed (s)') as HTMLInputElement).value).toBe('10')
  })

  it('shows a command’s only optional param in place, with no drawer', () => {
    render(<Stateful params={[{ name: 'queue_position', label: 'Queue position', type: 'int', required: false, minimum: 0, maximum: 10000 }]} inline />)
    expect(screen.getByLabelText('Queue position')).toBeTruthy()
    expect(screen.queryByRole('button', { name: /More settings/ })).toBeNull()
  })

  it('gives a described param an info tip, not inline text', () => {
    render(<Stateful params={flame} initial={{ speed: 5 }} />)
    const text = screen.getByText('How fast the flames move.')
    expect(text.hidden).toBe(true)
    const tip = screen.getByRole('button', { name: 'About Speed (s)' })
    expect(tip.getAttribute('aria-describedby')).toBe(text.id)
    fireEvent.focus(tip)
    expect(text.hidden).toBe(false)
    fireEvent.keyDown(tip, { key: 'Escape' })
    expect(text.hidden).toBe(true)
    expect(screen.queryAllByRole('button', { name: /^About / })).toHaveLength(1) // Power on has no description
  })
})
