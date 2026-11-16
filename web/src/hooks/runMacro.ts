import { api, toApiError } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Action, ActivityRun } from '@/api/types'
import { toast, toastError } from '@/components/ui/toast'
import { longestRunMs, waitMs } from '@/lib/activity'
import { sleep } from '@/lib/utils'

const POLL_MS = 1000

/**
 * Run a macro as it is on screen (saved or not) and say how it went: resolves once it has finished (it shows in
 * the runs) or we stop waiting. Only a problem is toasted; a clean run speaks for itself.
 */
export async function runMacro(actions: Action[], trigger: string, activityId: string | null): Promise<void> {
  let runId: number
  try {
    runId = (await api.runMacro({ actions, trigger, activity_id: activityId })).run_id
  } catch (e) {
    if (toApiError(e).code === 'busy') return toast('Too many macros waiting', { variant: 'error', description: 'Try again in a moment.' })
    return toastError('Couldn’t run the macro', e)
  }
  // It can't finish before its waits are over: check once they are, then every second until it could have taken
  // longest (each step at its timeout), and a little more.
  const until = Date.now() + longestRunMs(actions) + 3000
  await sleep(waitMs(actions))
  let run: ActivityRun | undefined
  const done = (r?: ActivityRun) => !!r && r.outcome !== 'running'
  while (!done(run) && Date.now() < until) {
    await sleep(POLL_MS)
    run = (await api.activityRuns().catch(() => null))?.runs.find((r) => r.id === runId)
  }
  invalidate('status')
  if (!run || !done(run)) return toast('The macro is still running', { description: 'See the Log for how it went.' })
  if (run.outcome === 'cancelled') toast('The macro was stopped', { description: 'An activity change stopped it partway.' })
  else if (run.outcome !== 'completed') toast('The macro ran with problems', { variant: 'error', description: 'See the Log for what went wrong.' })
}
