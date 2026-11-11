// What to tell the user after activate/off. The hub reports each phase
// separately: a transition that switched is never an error to retry, even
// when its startup (or shutdown) sequence couldn't run.
import type { TransitionResult } from '../api/types.ts'
import { humanize } from './utils.ts'

export interface TransitionMessage {
  variant: 'success' | 'warning'
  title: string
  description?: string
}

const REASONS: Record<string, string> = {
  queue_full: 'Hubbo was busy. Try again.',
}
const reasonText = (r?: string) => (r ? (REASONS[r] ?? `${humanize(r)}.`) : undefined)

export function describeTransition(r: TransitionResult, nameOf: (id: string) => string): TransitionMessage {
  if (r.active_activity_id) {
    const name = nameOf(r.active_activity_id)
    if (r.startup.status === 'rejected') return { variant: 'warning', title: `${name} is on, but its startup didn’t run`, description: reasonText(r.startup.reason) }
    return { variant: 'success', title: `${name} is on` }
  }
  if (r.shutdown.status === 'rejected') return { variant: 'warning', title: 'Turned off, but the shutdown didn’t run', description: reasonText(r.shutdown.reason) }
  return { variant: 'success', title: r.previous_activity_id ? 'Turned off' : 'Already off' }
}
