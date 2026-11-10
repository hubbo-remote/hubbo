import { api, errorMessage, toApiError } from '@/api/client'
import type { EntityDetailsStatus } from '@/api/types'
import { parseTrigger } from '@/lib/activity'
import { buttonLabel } from '@/lib/buttons'

/**
 * Register / re-link errors for an HA entity, in the admin's terms. The hub takes the
 * device's capabilities from the entity's loaded details (`details_required` without them).
 */
export async function haDeviceError(e: unknown): Promise<string> {
  const err = toApiError(e)
  if (err.code === 'details_required') return 'Home Assistant’s answer about this entity is out of date (its settings changed, or another entity was loaded since). Try again.'
  if (err.code === 'integration_unavailable') return 'Home Assistant isn’t connected right now, so Hubbo can’t read this entity. Check its state under Integrations and try again.'
  if (err.details.some((d) => d.code === 'unknown_device' && d.path === '/provider/entity_id')) return 'Hubbo offers no commands for this entity, so it can’t be a device.'
  // A re-link the hub refused because an activity's steps no longer fit: name those activities
  // (the hub reports them by index, so read the list only now).
  const refs = err.details.filter((d) => /^\/(activities\/\d+|global_bindings)\//.test(d.path))
  const names = refs.length ? ((await api.activities().catch(() => null))?.activities.map((a) => a.name) ?? []) : []
  const broken = refs.flatMap((d) => {
    const where = usedAt(d.path, names)
    return where ? [`• ${where}: ${d.message}`] : []
  })
  if (broken.length) return `That entity doesn’t offer what your activities use from this device:\n${broken.join('\n')}\nPick an entity with the same commands, or change those steps first.`
  return errorMessage(e, { details: true })
}

/** An entity's loaded details (§3b): what registering, re-linking or refreshing it snapshots. */
export type EntityDetails = EntityDetailsStatus & { state: 'done' }

/**
 * Load an entity's details (its attributes and its integration's services; about 2 s on the hub)
 * and wait for them. Throws with Home Assistant's reason when it fails. `stale()` stops the wait
 * when the caller moved on (e.g. picked another entity).
 */
export async function loadEntityDetails(integration: string, entityId: string, stale: () => boolean = () => false): Promise<EntityDetails | null> {
  let st = await api.entityDetailsStart(integration, entityId)
  const until = Date.now() + 60_000
  while (st.state === 'running') {
    if (Date.now() > until) throw new Error('Home Assistant is taking too long to describe this entity; try again')
    await new Promise((r) => setTimeout(r, 500))
    if (stale()) return null
    st = await api.entityDetails(integration, entityId)
  }
  if (stale()) return null
  if (st.state === 'done') return st as EntityDetails
  throw new Error(st.state === 'failed' ? (st.error ?? 'Home Assistant couldn’t describe this entity') : 'Another entity was loaded meanwhile; try again')
}

/**
 * Where a re-link breaks something, in the hub's paths (activities by index):
 * `/activities/1/startup/4/params/source` → "Watch TV, startup step 5";
 * `/global_bindings/up:down/0/…` → "Global buttons, Up down".
 */
function usedAt(path: string, names: string[]): string | null {
  const m = /^\/(?:activities\/(\d+)\/(startup|shutdown|bindings)|global_bindings)\/([^/]+)/.exec(path)
  if (!m) return null
  const owner = m[1] === undefined ? 'Global buttons' : (names[Number(m[1])] ?? `Activity ${Number(m[1]) + 1}`)
  const part = m[2] ?? 'bindings'
  if (part !== 'bindings') return `${owner}, ${part} step ${Number(m[3]) + 1}`
  const t = parseTrigger(decodeURIComponent(m[3]!))
  return `${owner}, ${t ? `${buttonLabel(t.button)} ${t.event}` : m[3]}`
}
