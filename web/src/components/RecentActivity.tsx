import { AlertTriangle, Check, CheckCheck, ChevronRight, CircleSlash, Loader2, Send, X } from 'lucide-react'
import { useState } from 'react'
import type { Action, ActivityRun, ActivityRunStep as RunStep } from '@/api/types'
import { actionParts } from '@/components/actions/describe'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { useActivities, useActivityRuns, useCatalog, useStatus } from '@/hooks/data'
import type { Catalog } from '@/lib/activity'
import { failedSteps, isFailure, runSummary, runTitle, stepReason } from '@/lib/runs'
import { agoMs, cn, duration } from '@/lib/utils'

/** The dashboard's last few startups and shutdowns: one line each, opening to its steps. */
export function RecentActivity() {
  const names = useActivities().data?.activities
  // Fetched again when /status shows a new run (its id) or the newest one finishing (its outcome).
  const last = useStatus(2000).data?.last_run
  const runs = useActivityRuns(last ? `${last.id}:${last.outcome}` : null).data?.runs
  const [open, setOpen] = useState<number | null>(null) // a run's id: a new run arriving doesn't move it
  if (!runs?.length) return null
  return (
    <Card className="mt-6" aria-label="Recent activity">
      <CardHeader>
        <CardTitle>Recent activity</CardTitle>
      </CardHeader>
      <CardContent>
        <ul className="grid gap-1.5">
          {runs.map((r) => {
            const failed = failedSteps(r) > 0 || r.outcome === 'rejected'
            const shown = open === r.id
            return (
              <li key={r.id} className={cn('rounded-lg border', failed && 'border-warning/60 bg-warning/5')}>
                <button
                  className="flex w-full items-center gap-2 rounded-lg px-3 py-2 text-left text-sm outline-none hover:bg-accent/50 focus-visible:ring-2 focus-visible:ring-ring"
                  aria-expanded={shown}
                  onClick={() => setOpen(shown ? null : r.id)}
                >
                  <ChevronRight className={cn('size-4 shrink-0 text-muted-foreground transition-transform', shown && 'rotate-90')} aria-hidden />
                  <span className="min-w-0 flex-1 truncate">
                    <span className="font-medium">{runTitle(r, names?.find((a) => a.id === r.activity_id)?.name ?? 'An activity')}</span>
                    <span className="text-muted-foreground"> · {agoMs(r.started_ago_ms)} · </span>
                    <span className={cn(failed ? 'font-medium' : 'text-muted-foreground')}>{runSummary(r)}</span>
                  </span>
                  {failed && <AlertTriangle className="size-4 shrink-0 text-warning" aria-hidden />}
                  {r.outcome === 'running' && <Loader2 className="size-4 shrink-0 animate-spin text-muted-foreground" aria-hidden />}
                </button>
                {shown && <RunSteps run={r} />}
              </li>
            )
          })}
        </ul>
      </CardContent>
    </Card>
  )
}

/** A run's steps, with the catalog (loaded only when one is opened) for their names. */
function RunSteps({ run }: { run: ActivityRun }) {
  const { catalog } = useCatalog()
  if (!run.steps.length) return <p className="border-t px-3 py-2 text-sm text-muted-foreground">No steps.</p>
  return (
    <ol className="grid gap-1 border-t px-3 py-2" aria-label="Steps">
      {run.steps.map((s) => (
        <StepLine key={s.index} step={s} catalog={catalog} />
      ))}
    </ol>
  )
}

const ICON: Record<RunStep['result'], { icon: typeof Check; className: string; label: string }> = {
  ok: { icon: Check, className: 'text-success', label: 'OK' },
  skipped: { icon: CheckCheck, className: 'text-success', label: 'Already set' },
  no_reply: { icon: Send, className: 'text-muted-foreground', label: 'Sent' },
  failed: { icon: X, className: 'text-destructive', label: 'Failed' },
  timed_out: { icon: X, className: 'text-destructive', label: 'Timed out' },
  cancelled: { icon: CircleSlash, className: 'text-muted-foreground', label: 'Skipped' },
  pending: { icon: Loader2, className: 'animate-spin text-muted-foreground', label: 'Running' },
}

function StepLine({ step, catalog }: { step: RunStep; catalog: Catalog }) {
  // Enough of an action to name it: a system step (no device) is Hubbo's own.
  const a: Action = { integration_id: step.device_id ? '' : 'system', device_id: step.device_id, command_id: step.command_id, params: {} }
  const { icon: Icon, className, label } = ICON[step.result]
  const reason = stepReason(step)
  const bad = isFailure(step)
  const p = actionParts(a, catalog)
  return (
    <li className="flex items-start gap-2 text-sm">
      <Icon className={cn('mt-0.5 size-4 shrink-0', className)} aria-label={label} />
      <div className="min-w-0 flex-1">
        <p className="truncate">
          {p.device && <span className="font-medium">{p.device} · </span>}
          {p.command}
        </p>
        {reason && <p className={cn('text-xs', bad ? 'text-destructive' : 'text-muted-foreground')}>{reason}</p>}
      </div>
      {step.duration_ms != null && !bad && step.result !== 'pending' && <span className="shrink-0 text-xs text-muted-foreground tabular-nums">{duration(step.duration_ms)}</span>}
    </li>
  )
}
