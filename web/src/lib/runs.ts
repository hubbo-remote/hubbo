// Activity runs (docs/design/activity-runs.md): the hub's last few startups and shutdowns, step by step, in plain
// words. Pure: imported by the UI and the mock, so relative imports only.
import type { Action, ActivityRun, ActivityRunStep as RunStep } from '../api/types.ts'
import { SYSTEM } from './activity.ts'
import { duration } from './utils.ts'

/** A step as an action, for the usual names and checks: a run keeps no params, and a step with no device is Hubbo's. */
export const stepAction = (s: RunStep): Action => ({ integration_id: s.device_id ? '' : SYSTEM, device_id: s.device_id, command_id: s.command_id, params: {} })

/** The run's activity by name, or a stand-in once it's been deleted. */
export const activityName = (activities: readonly { id: string; name: string }[] | undefined, id: string) => activities?.find((a) => a.id === id)?.name ?? 'An activity'

/** Why a step failed, as people say it (the hub's error codes; ExecuteResult `message` uses the same names). */
const REASONS: Record<string, string> = {
  adapter_timeout: 'no answer in time',
  adapter_unavailable: 'its integration isn’t connected',
  adapter_failed: 'the device refused it',
  entity_not_found: 'Home Assistant no longer has this device',
  state_unavailable: 'Home Assistant doesn’t know its current state',
  queue_full: 'Hubbo was too busy',
}

/** A skipped power step: what HA already showed. */
const ALREADY: Record<string, string> = { turn_on: 'already on', turn_off: 'already off' }

/**
 * What happened to a step in plain words, when there's more to say than a tick: "already on" (HA already showed it,
 * so nothing was sent), "sent, no reply", "timed out after 5 s", why it failed or was cancelled. Null for one that ran
 * fine or is running.
 */
export function stepReason(s: RunStep): string | null {
  if (s.result === 'skipped') return ALREADY[s.command_id.split('.')[1] ?? ''] ?? 'already set'
  if (s.result === 'no_reply') return 'sent, no reply'
  if (s.result === 'timed_out') return s.duration_ms != null ? `timed out after ${duration(s.duration_ms)}` : 'timed out'
  if (s.result === 'cancelled') return s.error === 'queue_full' ? 'skipped: Hubbo was too busy' : 'skipped: another activity took over'
  if (s.result !== 'failed') return null
  return (s.error && REASONS[s.error]) ?? (s.error ? `failed (${s.error.replace(/_/g, ' ')})` : 'failed')
}

/** A step that went wrong (not one that was already set, got no reply, or was cancelled before it ran). */
export const isFailure = (s: RunStep) => s.result === 'failed' || s.result === 'timed_out'

export const failedSteps = (r: ActivityRun) => r.steps.filter(isFailure).length

/** "start" or "stop": what the run did to its activity. */
export const runKind = (r: ActivityRun) => (r.kind === 'startup' ? 'start' : 'stop')

/** How the run went, as a log says it: "completed", "1 failed", "running…". */
export function runSummary(r: ActivityRun): string {
  const failed = failedSteps(r)
  if (r.outcome === 'running') return 'running…'
  if (r.outcome === 'rejected') return 'didn’t run'
  if (r.outcome === 'cancelled') return failed ? `cancelled, ${failed} failed` : 'cancelled'
  if (failed) return `${failed} failed`
  return r.steps.length ? 'completed' : 'nothing to do'
}

/**
 * The failing steps of an activity's startup or shutdown, by index, from its newest run if that was this `kind`: only
 * where the step there now is still the same command (an edit since the run moves or replaces steps, and then the
 * warning would point at the wrong one).
 */
export function failuresFor(run: ActivityRun | undefined, kind: ActivityRun['kind'], steps: readonly Action[]): Map<number, string> {
  const out = new Map<number, string>()
  for (const s of run?.kind === kind ? run.steps : []) {
    const reason = isFailure(s) ? stepReason(s) : null
    const now = steps[s.index]
    if (reason && now && now.command_id === s.command_id && now.device_id === s.device_id) out.set(s.index, reason)
  }
  return out
}
