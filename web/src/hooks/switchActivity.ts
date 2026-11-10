import { api, toApiError } from '@/api/client'
import { invalidate } from '@/api/hooks'
import { toast, toastError } from '@/components/ui/toast'
import { describeTransition } from '@/lib/transition'

/**
 * Activate an activity (or turn off) and say what happened phase by phase.
 * Only a failure that never reached the engine (503 busy) is offered as a retry.
 */
export async function switchActivity(id: string | 'off', nameOf: (id: string) => string) {
  try {
    const r = id === 'off' ? await api.off() : await api.activate(id)
    const m = describeTransition(r, nameOf)
    toast(m.title, { variant: m.variant, description: m.description })
  } catch (e) {
    const err = toApiError(e)
    if (err.code === 'busy') toast('Hubbo is busy', { variant: 'error', description: 'Try again in a moment.' })
    else toastError(id === 'off' ? 'Couldn’t turn off' : 'Couldn’t start the activity', e)
  } finally {
    invalidate('status', 'activities')
  }
}
