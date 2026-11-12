import { describe, expect, it } from 'vitest'
import { loadSeed } from '../../../mock/fixtures.ts'
import type { Catalog } from '@/lib/activity'
import { startAction, SYSTEM_VALUE } from './start'

const s = loadSeed()
const cat: Catalog = {
  devices: new Map(s.devices.map((d) => [d.id, d])),
  capabilities: s.capabilities,
  systemCommands: s.integrations.find((i) => i.id === 'system')!.commands!,
  activities: s.activities.map((a) => ({ id: a.id, name: a.name })),
}
const first = [...cat.devices.keys()][0]

describe('startAction', () => {
  it('starts on the asked device with its first command, else the first device', () => {
    const lg = startAction(cat, 'sequence', 'dev_lg_tv')!
    expect(lg.device_id).toBe('dev_lg_tv')
    expect(lg.command_id).not.toBe('')
    expect(startAction(cat, 'sequence')!.device_id).toBe(first)
    expect(startAction(cat, 'sequence', 'dev_gone')!.device_id).toBe(first)
  })

  it('starts on Hubbo only where Hubbo has commands (a binding), never on a wait', () => {
    expect(startAction(cat, 'binding', SYSTEM_VALUE)).toMatchObject({ integration_id: 'system', device_id: null })
    expect(startAction(cat, 'binding', SYSTEM_VALUE)!.command_id).not.toBe('wait')
    expect(startAction(cat, 'sequence', SYSTEM_VALUE)!.device_id).toBe(first)
  })

  it('falls back to Hubbo with no devices, or to nothing', () => {
    const empty = { ...cat, devices: new Map() }
    expect(startAction(empty, 'binding')).toMatchObject({ integration_id: 'system', device_id: null })
    expect(startAction(empty, 'sequence')).toBeNull()
  })
})
