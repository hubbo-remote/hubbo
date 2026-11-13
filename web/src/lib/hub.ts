import { api } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'

/** api/openapi.yaml HubName: 1–32 characters (the hub also refuses control characters). */
export const HUB_NAME_MAX = 32

/** The hub trims spaces (only spaces: a tab or newline is a control character it refuses). */
export const trimHubName = (name: string) => name.replace(/^ +| +$/g, '')

/** What's wrong with a name as typed (it's sent trimmed), or null; the hub has the last word. */
export function hubNameProblem(name: string): string | null {
  const chars = [...trimHubName(name)].length
  return !chars ? 'Name is required' : chars > HUB_NAME_MAX ? `At most ${HUB_NAME_MAX} characters` : null
}

/** Renames the hub (trimmed) and updates what shows it. */
export async function saveHubName(name: string) {
  const hub = await api.renameHub(trimHubName(name))
  setQueryData('hub', hub)
  invalidate('status')
  return hub
}
