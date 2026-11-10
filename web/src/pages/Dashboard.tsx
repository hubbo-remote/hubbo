import { AlertTriangle, Bluetooth, Cable, Home, Loader2, Power, Radio, type LucideIcon } from 'lucide-react'
import { useState, type ReactNode } from 'react'
import { Link } from 'wouter'
import type { IntegrationState, LatencyStats, Status } from '@/api/types'
import { ErrorState, HealthDot, PageHeader } from '@/components/common'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/misc'
import { switchActivity } from '@/hooks/switchActivity'
import { useActivities, useConfigLock, useIntegrations, useStatus } from '@/hooks/data'
import { buttonLabel } from '@/lib/buttons'
import { healthLabel } from '@/lib/health'
import { agoMs, bytes, cn, formatDuration, plural, timeAgo } from '@/lib/utils'

const TARGET_P95_MS = 10
const TARGET_P99_MS = 25

export function Dashboard() {
  const status = useStatus(2000)
  const integrations = useIntegrations()
  const s = status.data
  const integ = (type: string) => s?.integrations.find((i) => i.type === type)
  const detail = (type: string) => integrations.data?.find((i) => i.type === type)?.detail
  const ble = integ('ble_hid')
  const ha = integ('home_assistant')
  const ev = s?.remote.last_event

  return (
    <>
      <PageHeader title="Dashboard" description={s ? `${s.network.hostname ?? 'hubbo'} · up ${formatDuration(s.uptime_s)}` : 'Hub status'} />
      {status.error && !s ? (
        <ErrorState error={status.error} retry={status.refetch} />
      ) : (
        <div className="grid gap-4 lg:grid-cols-3">
          <ActivityCard status={s} className="lg:col-span-2" />
          <LatencyCard stats={s ? (s.diagnostics.dispatch_latency ?? null) : undefined} />
          <div className="grid gap-4 sm:grid-cols-2 lg:col-span-3 lg:grid-cols-4">
            <StatusTile
              icon={Radio}
              title="Remote"
              state={!s ? null : s.remote.paired ? (s.remote.last_packet_age_ms != null ? 'connected' : 'connecting') : 'not_configured'}
              value={s ? (s.remote.paired ? (s.remote.held?.length ? `Holding ${s.remote.held.map(buttonLabel).join(' + ')}` : 'Paired') : 'Not paired') : null}
              detail={
                !s ? null : !s.remote.paired ? (
                  <Link href="/settings/remote" className="text-primary underline-offset-4 hover:underline">
                    Pair a remote
                  </Link>
                ) : ev ? (
                  `${buttonLabel(ev.button)} ${ev.kind}${ev.timed_out ? ' (timed out)' : ''} · ${agoMs(ev.age_ms)}`
                ) : (
                  `No presses yet${s.remote.channel !== undefined ? ` · channel ${s.remote.channel}` : ''}`
                )
              }
            />
            <StatusTile icon={Bluetooth} title="Bluetooth" state={ble?.state ?? null} value={ble ? healthLabel(ble.state) : s ? 'Not available' : null} detail={detail('ble_hid') ?? ''} />
            <StatusTile icon={Home} title="Home Assistant" state={ha?.state ?? null} value={ha ? healthLabel(ha.state) : s ? 'Not available' : null} detail={detail('home_assistant') ?? ''} />
            <StatusTile
              icon={Cable}
              title="Ethernet"
              state={!s ? null : s.network.link === 'up' ? 'connected' : 'error'}
              value={s ? (s.network.ip ?? (s.network.link === 'up' ? 'No address' : 'Link down')) : null}
              detail={s?.network.hostname ? `${s.network.hostname}.local` : null}
            />
          </div>
          <ErrorsCard status={s} className="lg:col-span-2" />
          <SystemCard status={s} />
        </div>
      )}
    </>
  )
}

function ActivityCard({ status, className }: { status: Status | undefined; className?: string }) {
  const acts = useActivities()
  const [pending, setPending] = useState<string | null>(null)
  const lock = useConfigLock()
  const activeId = status?.active_activity_id ?? acts.data?.active_activity_id ?? null
  const list = acts.data?.activities
  const active = list?.find((a) => a.id === activeId)

  const run = async (id: string | 'off') => {
    setPending(id)
    await switchActivity(id, (x) => list?.find((a) => a.id === x)?.name ?? x)
    setPending(null)
  }

  return (
    <Card className={cn('overflow-hidden', className)}>
      <CardHeader>
        <div>
          <CardDescription>Current activity</CardDescription>
          {status ? <p className="mt-1 text-2xl font-semibold tracking-tight">{active?.name ?? (activeId ? activeId : 'Off')}</p> : <Skeleton className="mt-2 h-7 w-48" />}
        </div>
        <Button variant={activeId ? 'outline' : 'secondary'} onClick={() => run('off')} disabled={!activeId || !!pending || lock.state === 'blocked'} aria-label="Off: runs the current activity's shutdown">
          {pending === 'off' ? <Loader2 className="animate-spin" /> : <Power />}
          Off
        </Button>
      </CardHeader>
      <CardContent>
        <p className="mb-3 text-xs text-muted-foreground">Starting an activity runs only its startup sequence. Off runs the current activity's shutdown.</p>
        {list ? (
          list.length ? (
            <div className="grid grid-cols-2 gap-2 sm:grid-cols-3">
              {list.map((a) => {
                const on = a.id === activeId
                const n = Object.keys(a.bindings).length // keeps the validator out of the entry chunk
                return (
                  <button
                    key={a.id}
                    onClick={() => run(a.id)}
                    disabled={!!pending || lock.state === 'blocked'}
                    title={lock.state === 'blocked' ? lock.title : undefined}
                    aria-pressed={on}
                    className={cn(
                      'flex min-h-16 flex-col items-start justify-between gap-1 rounded-lg border p-3 text-left text-sm transition-colors outline-none hover:bg-accent focus-visible:ring-[3px] focus-visible:ring-ring/50 disabled:opacity-60 cursor-pointer',
                      on && 'border-primary bg-primary/10 hover:bg-primary/15',
                    )}
                  >
                    <span className="font-medium">{a.name}</span>
                    <span className="flex items-center gap-1.5 text-xs text-muted-foreground">
                      {pending === a.id ? <Loader2 className="size-3 animate-spin" /> : on ? <span className="size-1.5 rounded-full bg-success" /> : null}
                      {on ? 'Running' : `${plural(n, 'binding')}`}
                    </span>
                  </button>
                )
              })}
            </div>
          ) : (
            <p className="text-sm text-muted-foreground">
              No activities yet.{' '}
              <Link href="/activities/new" className="text-primary underline-offset-4 hover:underline">
                Create one
              </Link>
              .
            </p>
          )
        ) : (
          <div className="grid grid-cols-2 gap-2 sm:grid-cols-3">
            {[0, 1, 2].map((i) => (
              <Skeleton key={i} className="h-16" />
            ))}
          </div>
        )}
      </CardContent>
    </Card>
  )
}

function StatusTile({ icon: Icon, title, state, value, detail }: { icon: LucideIcon; title: string; state: IntegrationState | null; value: ReactNode; detail: ReactNode }) {
  return (
    <Card aria-label={title}>
      <CardContent className="grid gap-2 py-4">
        <div className="flex items-center justify-between">
          <span className="inline-flex items-center gap-2 text-sm text-muted-foreground">
            <Icon className="size-4" aria-hidden /> {title}
          </span>
          {state && (
            <span className="inline-flex items-center gap-1.5 text-xs text-muted-foreground">
              <HealthDot state={state} />
              <span className="sr-only">{healthLabel(state)}</span>
            </span>
          )}
        </div>
        {value != null ? <p className="truncate text-lg font-semibold">{value}</p> : <Skeleton className="h-6 w-28" />}
        <p className="min-h-4 truncate text-xs text-muted-foreground">{detail}</p>
      </CardContent>
    </Card>
  )
}

/** Percentiles on a 0–30 ms track with the p95/p99 targets marked. */
function LatencyMeter({ stats }: { stats: LatencyStats }) {
  const MAX = 30
  const pos = (v: number) => `${Math.min(100, (v / MAX) * 100)}%`
  const marks: [string, number, string][] = [
    ['p50', stats.p50_ms, 'bg-primary/60'],
    ['p95', stats.p95_ms, stats.p95_ms > TARGET_P95_MS ? 'bg-destructive' : 'bg-primary'],
    ['p99', stats.p99_ms, stats.p99_ms > TARGET_P99_MS ? 'bg-destructive' : 'bg-primary'],
  ]
  return (
    <figure className="grid gap-1" aria-label={`Dispatch latency p50 ${stats.p50_ms} ms, p95 ${stats.p95_ms} ms, p99 ${stats.p99_ms} ms; targets p95 under 10 ms, p99 under 25 ms`}>
      <div className="relative h-6" aria-hidden>
        <div className="absolute inset-x-0 top-2.5 h-1.5 overflow-hidden rounded-full bg-muted">
          <div className="absolute inset-y-0 left-0 bg-success/30" style={{ width: pos(TARGET_P95_MS) }} />
          <div className="absolute inset-y-0 bg-warning/30" style={{ left: pos(TARGET_P95_MS), width: `calc(${pos(TARGET_P99_MS)} - ${pos(TARGET_P95_MS)})` }} />
          <div className="absolute inset-y-0 right-0 bg-destructive/25" style={{ left: pos(TARGET_P99_MS) }} />
        </div>
        {marks.map(([k, v, c]) => (
          <span key={k} title={`${k} ${v} ms`} className={cn('absolute top-1 size-3.5 -translate-x-1/2 rounded-full ring-2 ring-card', c)} style={{ left: pos(v) }} />
        ))}
      </div>
      <div className="relative h-4 text-[10px] text-muted-foreground tabular-nums" aria-hidden>
        {[0, TARGET_P95_MS, TARGET_P99_MS].map((v) => (
          <span key={v} className={cn('absolute', v === 0 ? '' : '-translate-x-1/2')} style={{ left: pos(v) }}>
            {v} ms
          </span>
        ))}
      </div>
    </figure>
  )
}

function LatencyCard({ stats }: { stats: LatencyStats | null | undefined }) {
  const within = stats ? stats.p95_ms <= TARGET_P95_MS && stats.p99_ms <= TARGET_P99_MS : false
  const p = (label: string, v: number | undefined, target?: number) => (
    <div className="grid gap-0.5">
      <dt className="text-xs text-muted-foreground">{label}</dt>
      <dd className={cn('text-lg font-semibold tabular-nums', target && v && v > target && 'text-destructive')}>{v != null ? `${v.toFixed(1)} ms` : stats === null ? '–' : <Skeleton className="h-6 w-16" />}</dd>
    </div>
  )
  return (
    <Card>
      <CardHeader>
        <div>
          <CardTitle>RF dispatch latency</CardTitle>
          <CardDescription>Radio receive to adapter hand-off</CardDescription>
        </div>
        {stats && <Badge variant={within ? 'success' : 'destructive'}>{within ? 'Within target' : 'Over target'}</Badge>}
      </CardHeader>
      <CardContent className="grid gap-4">
        <dl className="grid grid-cols-3 gap-2">
          {p('p50', stats?.p50_ms)}
          {p('p95', stats?.p95_ms, TARGET_P95_MS)}
          {p('p99', stats?.p99_ms, TARGET_P99_MS)}
        </dl>
        {stats ? <LatencyMeter stats={stats} /> : stats === null ? <p className="text-sm text-muted-foreground">No button presses measured yet.</p> : <Skeleton className="h-10" />}
        <p className="text-xs text-muted-foreground">
          Targets p95 &lt; 10 ms, p99 &lt; 25 ms{stats && ` · ${stats.count.toLocaleString()} events`}
        </p>
      </CardContent>
    </Card>
  )
}

function ErrorsCard({ status, className }: { status: Status | undefined; className?: string }) {
  const errs = status?.diagnostics.last_errors ?? []
  return (
    <Card className={className}>
      <CardHeader>
        <div>
          <CardTitle>Recent errors</CardTitle>
          <CardDescription>Adapter failures, timeouts and recoveries</CardDescription>
        </div>
        <Link href="/settings/diagnostics" className="text-sm text-primary underline-offset-4 hover:underline">
          Diagnostics
        </Link>
      </CardHeader>
      <CardContent>
        {!status ? (
          <Skeleton className="h-16" />
        ) : errs.length === 0 ? (
          <p className="text-sm text-muted-foreground">No errors since boot.</p>
        ) : (
          <ul className="divide-y">
            {errs.slice(0, 6).map((e, i) => (
              <li key={i} className="flex gap-3 py-2.5 text-sm first:pt-0 last:pb-0">
                <AlertTriangle className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
                <div className="min-w-0 flex-1">
                  <p className="break-words">{e.message}</p>
                  <p className="text-xs text-muted-foreground">{[e.source, e.code, e.at ? timeAgo(e.at) : null].filter(Boolean).join(' · ')}</p>
                </div>
              </li>
            ))}
          </ul>
        )}
      </CardContent>
    </Card>
  )
}

function SystemCard({ status }: { status: Status | undefined }) {
  const d = status?.diagnostics
  const rows: [string, ReactNode][] = []
  if (status) {
    rows.push(['Firmware', status.hubbo.build ?? status.hubbo.version], ['Board', status.hubbo.board])
    if (d?.heap_free !== undefined) rows.push(['Heap free', `${bytes(d.heap_free)}${d.heap_min_free !== undefined ? ` (min ${bytes(d.heap_min_free)})` : ''}`])
    if (d?.psram_free !== undefined) rows.push(['PSRAM free', bytes(d.psram_free)])
    rows.push(['Queue overflows', <span className={d?.queue_overflows ? 'text-destructive' : undefined}>{d?.queue_overflows ?? 0}</span>])
  }
  return (
    <Card>
      <CardHeader>
        <CardTitle>System</CardTitle>
      </CardHeader>
      <CardContent>
        {status ? (
          <dl className="grid gap-2 text-sm">
            {rows.map(([k, v]) => (
              <div key={k} className="flex justify-between gap-3">
                <dt className="text-muted-foreground">{k}</dt>
                <dd className="truncate text-right font-medium tabular-nums">{v}</dd>
              </div>
            ))}
          </dl>
        ) : (
          <Skeleton className="h-28" />
        )}
      </CardContent>
    </Card>
  )
}
