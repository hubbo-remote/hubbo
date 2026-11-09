import { api, errorMessage, toApiError } from '@/api/client'

/**
 * Register / re-link errors for an HA entity, in the admin's terms. The hub takes the
 * device's capabilities from its latest discovery, so `unknown_device` means "not in it".
 */
export function haDeviceError(e: unknown): string {
  const err = toApiError(e)
  if (err.code === 'integration_unavailable') return 'Home Assistant isn’t connected right now, so Hubbo can’t read this entity. Check its state under Integrations and try again.'
  if (err.details.some((d) => d.code === 'unknown_device' && d.path === '/provider/entity_id'))
    return 'Home Assistant’s latest discovery doesn’t include this entity, or offers no commands for it. Discover again and pick it from the fresh list.'
  return errorMessage(e, { details: true })
}

/** Start a discovery and wait for it (the hub reads states, services and the registry). */
export async function runDiscovery(integration: string): Promise<void> {
  await api.discoverStart(integration)
  const until = Date.now() + 60_000
  for (;;) {
    const st = await api.discovery(integration, { limit: 1 })
    if (st.state === 'done') return
    if (st.state === 'failed') throw new Error(st.error ?? 'Discovery failed')
    if (st.state === 'idle') throw new Error('Discovery was cancelled')
    if (Date.now() > until) throw new Error('Discovery is taking too long; try again')
    await new Promise((r) => setTimeout(r, 1000))
  }
}
