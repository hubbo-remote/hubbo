import type { Action } from '@/api/types'
import { commandFor, isSystem, type Catalog } from '@/lib/activity'
import { keyLabel } from '@/lib/keys'

export function commandLabel(a: Action, cat: Catalog): string {
  return commandFor(a, cat)?.label ?? a.command_id
}

export function paramSummary(a: Action, cat?: Catalog): string {
  if (isSystem(a) && a.command_id === 'wait') {
    const d = Number(a.params.duration_ms ?? 0)
    return d >= 1000 ? `${(d / 1000).toFixed(d % 1000 ? 1 : 0)} s` : `${d} ms`
  }
  return Object.entries(a.params)
    .map(([k, v]) => {
      if (k === 'activity_id' && typeof v === 'string') return cat?.activities.find((x) => x.id === v)?.name ?? v
      if (typeof v === 'boolean') return v ? k.replace(/_/g, ' ') : `not ${k.replace(/_/g, ' ')}`
      if (k === 'key' && typeof v === 'string') return keyLabel(v).toLowerCase()
      if (k === 'level' && typeof v === 'number' && v <= 1) return `${Math.round(v * 100)}%`
      return String(v)
    })
    .join(', ')
}

export function deviceName(a: Action, cat: Catalog): string {
  if (isSystem(a)) return 'Hubbo'
  const d = a.device_id ? cat.devices.get(a.device_id) : undefined
  return d?.name ?? `${a.device_id} (missing)`
}
