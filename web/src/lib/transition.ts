// What to tell the user after activate/off. The hub reports each phase
// separately: a transition that switched is never an error to retry, even
// when its startup (or shutdown) sequence couldn't run.
import type { TransitionResult } from '../api/types.ts'

export interface TransitionMessage {
  variant: 'success' | 'warning'
  title: string
  description?: string
}

const REASONS: Record<string, string> = {
  queue_full: 'the action queue was full',
}
const reasonText = (r?: string) => (r ? (REASONS[r] ?? r.replace(/_/g, ' ')) : 'no reason given')
const steps = (n: number) => `${n} step${n === 1 ? '' : 's'}`

export function describeTransition(r: TransitionResult, nameOf: (id: string) => string): TransitionMessage {
  if (r.active_activity_id) {
    const name = nameOf(r.active_activity_id)
    if (r.startup.status === 'rejected') return { variant: 'warning', title: `Switched to ${name}, but its startup didn’t run`, description: `Reason: ${reasonText(r.startup.reason)}.` }
    return { variant: 'success', title: `Switched to ${name}`, description: r.startup.queued_actions ? `${steps(r.startup.queued_actions)} queued` : undefined }
  }
  if (r.shutdown.status === 'rejected') return { variant: 'warning', title: 'Turned off, but the shutdown didn’t run', description: `Reason: ${reasonText(r.shutdown.reason)}.` }
  return { variant: 'success', title: r.previous_activity_id ? 'Turned off' : 'Already off', description: r.shutdown.queued_actions ? `${steps(r.shutdown.queued_actions)} queued` : undefined }
}
