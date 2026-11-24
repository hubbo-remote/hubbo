import { Box, Check, CircleSlash, History, Hourglass, Layers, Lightbulb, Loader2, MonitorPlay, Play, Power, Radio, Send, SkipForward, ToggleRight, X, type LucideIcon } from 'lucide-react'
import type { ReactNode } from 'react'
import type { ActivityRun, ActivityRunStep as RunStep, Device } from '@/api/types'
import { actionParts } from '@/components/actions/describe'
import { Badge } from '@/components/ui/badge'
import { Skeleton } from '@/components/ui/misc'
import { useActivities, useActivityRuns, useCatalog } from '@/hooks/data'
import { domainOf } from '@/lib/commands'
import { INTEGRATION_ICON } from '@/lib/health'
import { activityName, failedSteps, isFailure, runKind, runSummary, stepAction, stepReason } from '@/lib/runs'
import { isWait, type Catalog } from '@/lib/activity'
import { agoMs, cn, duration, plural } from '@/lib/utils'

/**
 * The hub's last few startups, shutdowns and button macros as a timeline, newest first: each run is a node on a rail at the left,
 * its steps hang off the same rail. Every part of a line is its own token; screen readers still hear each line as
 * one sentence (the joiners are sr-only, the glyphs aria-hidden). With `activityId`, only that activity's runs, and
 * no name on each (the page already says it).
 */
export function ActivityLog({ activityId }: { activityId?: string }) {
  const names = useActivities().data?.activities
  const { catalog } = useCatalog()
  const runs = useActivityRuns()
  if (!runs.data) return <Skeleton className="h-40" />
  const shown = activityId ? runs.data.runs.filter((r) => r.activity_id === activityId) : runs.data.runs
  if (!shown.length) {
    return (
      <p className="flex items-center gap-2 py-6 text-sm text-muted-foreground">
        <History className="size-4 shrink-0" aria-hidden /> Nothing yet. Start {activityId ? 'this activity' : 'an activity'} and each step shows here.
      </p>
    )
  }
  return (
    <ol aria-label="Activity log" className="text-sm">
      {shown.map((r, i) => (
        <RunEntry key={r.id} run={r} newest={i === 0} name={activityId ? null : activityName(names, r.activity_id)} catalog={catalog} />
      ))}
    </ol>
  )
}

/** Read as a pause between tokens, never seen. */
const Sr = ({ children }: { children: ReactNode }) => <span className="sr-only">{children}</span>

/** An icon and its colours; `spin` for one still running. */
type Glyph = { icon: LucideIcon; className: string; spin?: boolean }

/** The run's node on the rail: how it went at a glance. */
function runNode(run: ActivityRun, failed: boolean): Glyph {
  if (run.outcome === 'running') return { icon: Loader2, className: 'bg-muted text-muted-foreground', spin: true }
  if (failed) return { icon: X, className: 'bg-destructive/15 text-destructive' }
  if (run.outcome === 'cancelled') return { icon: CircleSlash, className: 'bg-muted text-muted-foreground' }
  return { icon: Check, className: 'bg-success/15 text-success' }
}

/** "5 steps · 2 skipped · 1.8 s": the run's shape in one quiet line. */
function runFacts(run: ActivityRun): string {
  const skipped = run.steps.filter((s) => s.result === 'skipped').length
  const parts = [plural(run.steps.length, 'step')]
  if (skipped) parts.push(`${skipped} skipped`)
  if (run.duration_ms != null) parts.push(duration(run.duration_ms))
  return parts.join(' · ')
}

function RunEntry({ run, newest, name, catalog }: { run: ActivityRun; newest: boolean; name: string | null; catalog: Catalog }) {
  const failed = failedSteps(run) > 0 || run.outcome === 'rejected'
  const node = runNode(run, failed)
  const Kind = run.kind === 'macro' ? Layers : run.kind === 'startup' ? Play : Power
  return (
    <li className="relative pb-6 pl-9 last:pb-0">
      {/* The rail: from under this node down to the next one. */}
      <span className="absolute top-7 bottom-0 left-[13px] w-px bg-border" aria-hidden />
      <span className={cn('absolute top-0 left-0 grid size-7 place-items-center rounded-full ring-4 ring-background', node.className)} aria-hidden>
        <node.icon className={cn('size-3.5', node.spin && 'animate-spin')} strokeWidth={2.5} />
      </span>
      <div className="flex min-h-7 flex-wrap items-center gap-x-2 gap-y-1">
        {name && (
          <>
            <span className={cn('min-w-0 font-semibold break-words', newest && 'text-base')}>{name}</span>
            <Sr> </Sr>
          </>
        )}
        <Badge variant="outline" className="font-normal text-muted-foreground">
          <Kind aria-hidden />
          {runKind(run)}
        </Badge>
        <span className="ml-auto flex items-center gap-2">
          <Sr>, </Sr>
          <span className="font-mono text-xs text-muted-foreground tabular-nums">{agoMs(run.started_ago_ms)}</span>
          <Sr>, </Sr>
          <Badge variant={failed ? 'destructive' : run.outcome === 'completed' ? 'success' : 'secondary'}>{runSummary(run)}</Badge>
        </span>
      </div>
      <p className="text-xs text-muted-foreground">{runFacts(run)}</p>
      {run.steps.length > 0 && (
        // From sm up the steps share columns (device | command | result), so they line up down the run; a phone wraps.
        <ol className="mt-2 grid gap-px sm:grid-cols-[max-content_minmax(0,1fr)_auto]" aria-label="Steps">
          {run.steps.map((s) => (
            <StepLine key={s.index} step={s} catalog={catalog} />
          ))}
        </ol>
      )}
    </li>
  )
}

/** A step's glyph on the rail. */
function stepGlyph(step: RunStep): Glyph {
  if (step.result === 'pending') return { icon: Loader2, className: 'text-muted-foreground', spin: true }
  if (isFailure(step)) return { icon: X, className: 'text-destructive' }
  if (step.result === 'skipped') return { icon: SkipForward, className: 'text-muted-foreground' }
  if (step.result === 'no_reply') return { icon: Send, className: 'text-muted-foreground' }
  if (step.result === 'cancelled') return { icon: CircleSlash, className: 'text-muted-foreground' }
  if (isWait(stepAction(step))) return { icon: Hourglass, className: 'text-muted-foreground' }
  return { icon: Check, className: 'text-success/70' }
}

/** A device's kind at a glance, from where it lives: its HA entity's domain, or its platform (Bluetooth, infrared). */
const DOMAIN_ICON: Record<string, LucideIcon> = { media_player: MonitorPlay, remote: Radio, light: Lightbulb, switch: ToggleRight }
function deviceIcon(d: Device | undefined): LucideIcon {
  if (d && d.provider.type !== 'home_assistant') return INTEGRATION_ICON[d.platform]
  return DOMAIN_ICON[domainOf(d) ?? ''] ?? Box
}

function StepLine({ step, catalog }: { step: RunStep; catalog: Catalog }) {
  // A run step carries no params, so a wait reads "Wait" and its duration says how long.
  const p = actionParts(stepAction(step), catalog)
  const bad = isFailure(step)
  const glyph = stepGlyph(step)
  const DeviceIcon = deviceIcon(step.device_id ? catalog.devices.get(step.device_id) : undefined)
  return (
    <li className="relative -mx-1.5 flex flex-wrap items-center gap-x-2 gap-y-1 rounded-md px-1.5 py-0.5 hover:bg-muted/40 sm:col-span-3 sm:grid sm:grid-cols-subgrid">
      {/* The glyph sits on the rail, centred 14px into the run (this row starts 30px in), masked so the rail breaks around it. */}
      <span className="absolute top-1/2 -left-6 grid size-4 -translate-y-1/2 place-items-center rounded-full bg-background" aria-hidden>
        <glyph.icon className={cn('size-3', glyph.className, glyph.spin && 'animate-spin')} strokeWidth={2.5} />
      </span>
      {p.device ? (
        <>
          <Badge variant="outline" className={cn('justify-self-start font-normal whitespace-normal', bad && 'border-destructive/50 text-destructive')}>
            <DeviceIcon aria-hidden />
            {p.device}
          </Badge>
          <Sr>: </Sr>
        </>
      ) : (
        <span className="hidden sm:block" aria-hidden /> // a wait: an empty device column keeps the commands aligned
      )}
      <span className={cn('min-w-0 break-words', bad && 'font-medium text-destructive')}>{p.command}</span>
      <Sr>, </Sr>
      <span className="ml-auto shrink-0 sm:ml-0 sm:justify-self-end">
        <StepResult step={step} bad={bad} />
      </span>
    </li>
  )
}

/** How a step ended, at the right: a pill with the reason (or running…), else how long it took. */
function StepResult({ step, bad }: { step: RunStep; bad: boolean }) {
  if (step.result === 'pending') return <Badge variant="secondary" className="font-normal">running…</Badge>
  const reason = stepReason(step)
  if (reason) {
    return (
      <Badge variant={bad ? 'destructive' : 'secondary'} className={cn('whitespace-normal', !bad && 'font-normal text-muted-foreground')}>
        {reason}
      </Badge>
    )
  }
  return step.duration_ms != null ? <span className="font-mono text-xs text-muted-foreground tabular-nums">{duration(step.duration_ms)}</span> : null
}
