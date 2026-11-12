import type { Action } from '@/api/types'
import { hubboCommands, SYSTEM, type Catalog } from '@/lib/activity'
import { actionFor, choices, domainOf } from '@/lib/commands'

/** The device picker's value for Hubbo itself. */
export const SYSTEM_VALUE = '__system__'

/** Where an action on a picked device (or Hubbo) goes. */
export const targetOf = (catalog: Catalog, d: string) =>
  d === SYSTEM_VALUE ? { integration_id: SYSTEM, device_id: null } : { integration_id: catalog.devices.get(d)!.integration_id, device_id: d }

/** A fresh action on a device (its first command) or on Hubbo, else on the first device; null when there's nothing to pick. */
export function startAction(catalog: Catalog, context: 'sequence' | 'binding', deviceId?: string | null): Action | null {
  const hubbo = hubboCommands(catalog, context)
  let d = deviceId ?? null
  if (d === SYSTEM_VALUE ? !hubbo.length : !d || !catalog.devices.has(d)) d = [...catalog.devices.keys()][0] ?? (hubbo.length ? SYSTEM_VALUE : null)
  if (!d) return null
  const target = targetOf(catalog, d)
  const cmds = d === SYSTEM_VALUE ? hubbo : (catalog.capabilities.get(d)?.commands ?? [])
  const first = choices(cmds, context, domainOf(catalog.devices.get(d)))[0]
  return first ? actionFor(first.id, target, cmds) : { ...target, command_id: '', params: {} }
}
