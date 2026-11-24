// Named queries. Keys are prefixes for invalidate().
import { useMemo, useState } from 'react'
import { api } from '@/api/client'
import { setQueryData, useQuery } from '@/api/hooks'
import type { Capabilities, DeviceDetail, HubUpdate, SmartSlotActions } from '@/api/types'
import { toastError } from '@/components/ui/toast'
import type { Catalog } from '@/lib/activity'

export const useSession = () => useQuery('session', () => api.session())
/** `enabled: false` skips the request (the hub's settings need the admin's session). */
export const useHub = (enabled = true) => useQuery(enabled ? 'hub' : null, () => api.hub())
/** Saves hub settings as they change (a switch, a select): `saving` while it's in flight, a toast if it fails. */
export function useSaveHub() {
  const [saving, setSaving] = useState(false)
  const save = async (patch: HubUpdate) => {
    setSaving(true)
    try {
      setQueryData('hub', await api.updateHub(patch))
    } catch (e) {
      toastError('Couldn’t save the setting', e)
    } finally {
      setSaving(false)
    }
  }
  return { save, saving }
}
/** `enabled: false` skips the request (e.g. before there's a session to make it with). */
export const useStatus = (pollMs = 3000, enabled = true) => useQuery(enabled ? 'status' : null, () => api.status(), { pollMs })
export const useDevices = (enabled = true) => useQuery(enabled ? 'devices' : null, () => api.devices())
export const useDevice = (id: string) => useQuery(`devices/${id}`, () => api.device(id))
export const useCapabilities = (id: string | null) => useQuery(id ? `caps/${id}` : null, () => api.capabilities(id!))
export const useActivities = () => useQuery('activities', () => api.activities())
export const useActivity = (id: string | null) => useQuery(id ? `activity/${id}` : null, () => api.activity(id!))
export const useGlobalBindings = () => useQuery('global-bindings', () => api.globalBindings())
export const useSmartButtons = () => useQuery('smart-buttons', () => api.smartButtons())
/**
 * The last few startups and shutdowns (GET /activity-runs), fetched again only when /status (polled anyway) shows a
 * new newest run or that run finishing: its `last_run` id and outcome are the key. Nothing until /status is in.
 */
export function useActivityRuns() {
  const status = useStatus().data
  const last = status?.last_run
  return useQuery(status ? `activity-runs/${last ? `${last.id}:${last.outcome}` : 'none'}` : null, () => api.activityRuns(), { keepPrevious: true })
}

/** What the slots would do with these devices (picked, not saved yet), by device id. */
export function useSmartActions(ids: string[]) {
  return useQuery(
    ids.length ? `smart-actions/${ids.join(',')}` : null,
    async () => new Map<string, SmartSlotActions>(await Promise.all(ids.map(async (id) => [id, await api.smartButtonActions(id)] as const))),
    { keepPrevious: true },
  )
}
export const useIntegrations = (pollMs?: number) => useQuery('integrations', () => api.integrations(), { pollMs })
export const useProfiles = (enabled = true) => useQuery(enabled ? 'profiles' : null, () => api.profiles())
export const useKinds = (enabled = true) => useQuery(enabled ? 'kinds' : null, () => api.kinds())
export const useIntegrationConfig = (id: string) => useQuery(`integration-config/${id}`, () => api.integrationConfig(id))

/** Everything the binding/sequence editors validate against: devices, their commands, system commands, activities. */
/** A Retry for a page built on several queries: refetches each one that failed. */
export const retryFailed =
  (...queries: { error?: unknown; refetch: () => Promise<void> }[]) =>
  async () => {
    await Promise.all(queries.filter((q) => q.error).map((q) => q.refetch()))
  }

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
  // Profile devices' (Bluetooth, IR) commands, for the ones with their own timed hold.
  const profileIds = (devices.data ?? []).filter((d) => d.profile).map((d) => d.id).sort()
  const details = useDeviceDetails(profileIds)
  const catalog = useMemo<Catalog>(
    () => ({
      devices: new Map((devices.data ?? []).map((d) => [d.id, d])),
      capabilities: new Map((caps.data ?? []).map((c: Capabilities) => [c.device_id, c])),
      systemCommands: integrations.data?.find((i) => i.type === 'system')?.commands ?? [],
      activities: (activities.data?.activities ?? []).map((a) => ({ id: a.id, name: a.name })),
      timed: new Map([...(details.data?.values() ?? [])].map((d) => [d.id, new Set(Object.entries(d.commands ?? {}).filter(([, c]) => c.hold_ms).map(([n]) => n))])),
    }),
    [devices.data, caps.data, integrations.data, activities.data, details.data],
  )
  const loading = devices.loading || caps.loading || integrations.loading || activities.loading
  return { catalog, loading, error: devices.error ?? caps.error ?? integrations.error, refetch: retryFailed(devices, caps, integrations, activities, details), devices: devices.data ?? [] }
}

/** These devices in the detail form, fetched together; seeds the per-device keys the device pages use. */
export function useDeviceDetails(ids: string[]) {
  return useQuery(ids.length ? `device-details/${ids.join(',')}` : null, async () => {
    const list = await Promise.all(ids.map((id) => api.device(id)))
    for (const d of list) setQueryData(`devices/${d.id}`, d)
    return new Map<string, DeviceDetail>(list.map((d) => [d.id, d]))
  })
}

/**
 * Whether the hub refuses edits right now (/status config: `blocked` or
 * `storage_unavailable`). Editing controls disable themselves instead of
 * letting saves fail one by one; the banner explains the way out.
 */
export function useConfigLock() {
  const c = useStatus().data?.config
  const locked = c?.state === 'blocked' || c?.state === 'storage_unavailable'
  return { locked, state: c?.state, title: locked ? 'Changes are paused' : undefined }
}
