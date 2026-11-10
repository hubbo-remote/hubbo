// Named queries. Keys are prefixes for invalidate().
import { useMemo } from 'react'
import { api } from '@/api/client'
import { setQueryData, useQuery } from '@/api/hooks'
import type { Capabilities } from '@/api/types'
import type { Catalog } from '@/lib/activity'

export const useSession = () => useQuery('session', () => api.session())
/** `enabled: false` skips the request (e.g. before there's a session to make it with). */
export const useStatus = (pollMs = 3000, enabled = true) => useQuery(enabled ? 'status' : null, () => api.status(), { pollMs })
export const useDevices = (enabled = true) => useQuery(enabled ? 'devices' : null, () => api.devices())
export const useDevice = (id: string) => useQuery(`devices/${id}`, () => api.device(id))
export const useCapabilities = (id: string | null) => useQuery(id ? `caps/${id}` : null, () => api.capabilities(id!))
export const useActivities = () => useQuery('activities', () => api.activities())
export const useActivity = (id: string | null) => useQuery(id ? `activity/${id}` : null, () => api.activity(id!))
export const useGlobalBindings = () => useQuery('global-bindings', () => api.globalBindings())
export const useIntegrations = (enabled = true) => useQuery(enabled ? 'integrations' : null, () => api.integrations())
export const useIntegrationConfig = (id: string) => useQuery(`integration-config/${id}`, () => api.integrationConfig(id))

/** Everything the binding/sequence editors validate against: devices, their commands, system commands, activities. */
export function useCatalog() {
  const devices = useDevices()
  const integrations = useIntegrations()
  const activities = useActivities()
  const ids = devices.data?.map((d) => d.id).join(',') ?? ''
  const caps = useQuery(ids ? `caps-all/${ids}` : null, async () => {
    const list = await Promise.all((devices.data ?? []).map((d) => api.capabilities(d.id)))
    // Seed the per-device keys so pickers and device pages don't fetch them again.
    for (const c of list) setQueryData(`caps/${c.device_id}`, c)
    return list
  })
  const catalog = useMemo<Catalog>(
    () => ({
      devices: new Map((devices.data ?? []).map((d) => [d.id, d])),
      capabilities: new Map((caps.data ?? []).map((c: Capabilities) => [c.device_id, c])),
      systemCommands: integrations.data?.find((i) => i.type === 'system')?.commands ?? [],
      activities: (activities.data?.activities ?? []).map((a) => ({ id: a.id, name: a.name })),
    }),
    [devices.data, caps.data, integrations.data, activities.data],
  )
  const loading = devices.loading || caps.loading || integrations.loading || activities.loading
  return { catalog, loading, error: devices.error ?? caps.error ?? integrations.error, devices: devices.data ?? [] }
}

/**
 * Whether the hub refuses edits right now (/status config: `blocked` or
 * `storage_unavailable`). Editing controls disable themselves instead of
 * letting saves fail one by one; the banner explains the way out.
 */
export function useConfigLock() {
  const c = useStatus().data?.config
  const locked = c?.state === 'blocked' || c?.state === 'storage_unavailable'
  return { locked, state: c?.state, reason: c?.reason, title: locked ? 'The hub isn’t accepting changes right now (see the banner at the top)' : undefined }
}
