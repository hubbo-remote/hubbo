// @vitest-environment jsdom
import { fireEvent, render, screen } from '@testing-library/react'
import { describe, expect, it, vi } from 'vitest'
import type { ParamSpec } from '@/api/types'
import { ParamForm } from './ParamForm'

const select = (label: RegExp) => screen.getByLabelText(label) as HTMLSelectElement
const shown = (el: HTMLSelectElement) => el.selectedOptions[0]?.textContent

describe('ParamForm selects show exactly what is stored', () => {
  it('a required activity ref that is unset shows "Choose…", and picking the only activity sets it', () => {
    const onChange = vi.fn()
    const p: ParamSpec = { name: 'activity_id', type: 'activity_ref', required: true }
    render(<ParamForm params={[p]} values={{}} onChange={onChange} activities={[{ id: 'act_tv', name: 'Watch TV' }]} />)
    expect(select(/Activity id/).value).toBe('')
    expect(shown(select(/Activity id/))).toBe('Choose…')
    fireEvent.change(select(/Activity id/), { target: { value: 'act_tv' } })
    expect(onChange).toHaveBeenCalledWith({ activity_id: 'act_tv' })
  })

  it('an optional enum holding a value that is no longer an option shows it, and "Not set" clears it', () => {
    const onChange = vi.fn()
    const p: ParamSpec = { name: 'source', type: 'enum', required: false, options: ['HDMI 1', 'HDMI 2'] }
    render(<ParamForm params={[p]} values={{ source: 'Phono' }} onChange={onChange} />)
    expect(select(/Source/).value).toBe('Phono')
    expect(shown(select(/Source/))).toBe('Phono (not available)')
    fireEvent.change(select(/Source/), { target: { value: '' } })
    expect(onChange).toHaveBeenCalledWith({})
  })

  it('a stored activity ref is shown as stored while the activities are still loading', () => {
    const p: ParamSpec = { name: 'activity_id', type: 'activity_ref', required: true }
    render(<ParamForm params={[p]} values={{ activity_id: 'act_tv' }} onChange={() => {}} activities={[]} />)
    expect(select(/Activity id/).value).toBe('act_tv')
    expect(shown(select(/Activity id/))).toBe('act_tv')
  })
})
