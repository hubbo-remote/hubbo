// @vitest-environment jsdom
import { render, screen } from '@testing-library/react'
import { describe, expect, it } from 'vitest'
import type { Device } from '@/api/types'
import type { Catalog } from '@/lib/activity'
import { DeviceSelect } from './DeviceSelect'

const cat: Catalog = { devices: new Map([['dev_tv', { id: 'dev_tv', name: 'TV' } as Device]]), capabilities: new Map(), systemCommands: [], activities: [] }
const shown = () => (screen.getByRole('combobox') as HTMLSelectElement).selectedOptions[0]?.textContent

describe('DeviceSelect', () => {
  it('shows a removed device as unknown, not as the first device', () => {
    render(<DeviceSelect id="d" value="dev_gone" onChange={() => {}} catalog={cat} hubbo />)
    expect(shown()).toBe('Unknown device')
    expect(screen.getAllByRole('option').map((o) => o.textContent)).toEqual(['Unknown device', 'TV', 'Hubbo'])
  })
})
