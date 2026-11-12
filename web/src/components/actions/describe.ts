import type { Action } from '@/api/types'
import { commandFor, isSystem, LEVEL_SCALE, numberChoices, percentLabel, type Catalog } from '@/lib/activity'
import { commandTitle, friendlyName, namedParam, toggleLabel, toggleOf } from '@/lib/commands'
import { paramValueLabel } from '@/lib/keys'

export function commandLabel(a: Action, cat: Catalog): string {
  const cmd = commandFor(a, cat)
  const t = toggleOf(a, cmd)
  return friendlyName(a) ?? (t ? toggleLabel(cmd!, ...t) : cmd && commandTitle(cmd)) ?? a.command_id
}

/** 500 ms, 1 s, 1.5 s, 2.25 s: milliseconds under a second, else seconds without trailing zeros. */
const duration = (ms: number) => (ms >= 1000 ? `${Number((ms / 1000).toFixed(3))} s` : `${ms} ms`)

export function paramSummary(a: Action, cat?: Catalog): string {
  if (isSystem(a) && a.command_id === 'wait') return duration(Number(a.params.duration_ms ?? 0))
  const cmd = cat && commandFor(a, cat)
  const named = friendlyName(a) ? namedParam(a) : cmd ? toggleOf(a, cmd)?.[0] : undefined // already in the command's name
  return Object.entries(a.params)
    .filter(([k]) => k !== named)
    .map(([k, v]) => {
      if (k === 'activity_id' && typeof v === 'string') return cat?.activities.find((x) => x.id === v)?.name ?? v
      if (typeof v === 'boolean') return v ? k.replace(/_/g, ' ') : `not ${k.replace(/_/g, ' ')}`
      if (k === 'hold_ms' && typeof v === 'number') return `held ${duration(v)}`
      const spec = cmd && cmd.params.find((p) => p.name === k)
      if (typeof v === 'number') {
        const choices = spec ? numberChoices(a.command_id, spec) : null
        if (choices) return choices.phrase(v, commandTitle(cmd!))
        if (!spec && k === 'level' && v <= 1) return percentLabel(LEVEL_SCALE, v)
      }
      return typeof v === 'string' ? paramValueLabel(k, v) : String(v)
    })
    .join(', ')
}

export function deviceName(a: Action, cat: Catalog): string {
  if (isSystem(a)) return 'Hubbo'
  const d = a.device_id ? cat.devices.get(a.device_id) : undefined
  return d?.name ?? `${a.device_id} (missing)`
}
