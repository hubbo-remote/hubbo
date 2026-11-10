import { AlertTriangle, Bluetooth, Cable, Home, Radio, RefreshCw, type LucideIcon } from 'lucide-react'
import type { ReactNode } from 'react'
import { Link, useLocation } from 'wouter'
import type { IntegrationState, LatencyStats, Status as HubStatus } from '@/api/types'
import { Backups } from '@/components/admin/Backups'
import { FirmwareUpdate } from '@/components/admin/FirmwareUpdate'
import { ErrorState, Facts, HealthDot, PageHeader, Section } from '@/components/common'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Skeleton } from '@/components/ui/misc'
import { Tabs } from '@/components/ui/tabs'
import { useIntegrations, useStatus } from '@/hooks/data'
import { buttonLabel } from '@/lib/buttons'
import { EVENT_VERB } from '@/lib/remote'
import { healthLabel } from '@/lib/health'
import { agoMs, bytes, cn, formatDuration, timeAgo } from '@/lib/utils'
import { NotFound } from './NotFound'

const TABS = [
  { id: 'overview', label: 'Overview' },
  { id: 'backups', label: 'Backups' },
  { id: 'firmware', label: 'Firmware' },
  { id: 'diagnostics', label: 'Diagnostics' },
]

export default function Status({ tab = 'overview' }: { tab?: string }) {
  const [, navigate] = useLocation()
  // Overview is the live view; other tabs ride the app shell's 3 s poll and add no timer of their own.
  const status = useStatus(tab === 'overview' ? 2000 : undefined)
  const s = status.data
  if (!TABS.some((t) => t.id === tab)) return <NotFound />
  return (
    <>
      <PageHeader title="Status" description={s ? `${s.network.hostname ?? 'hubbo'} · up ${formatDuration(s.uptime_s)}` : undefined} />
      <Tabs tabs={TABS} value={tab} onChange={(t) => navigate(`/status/${t}`, { replace: true })}>
        {tab === 'overview' && (status.error && !s ? <ErrorState error={status.error} retry={status.refetch} /> : <Overview status={s} />)}
        {tab === 'backups' && <Backups />}
        {tab === 'firmware' && <FirmwareUpdate />}
        {tab === 'diagnostics' && <Diagnostics status={s} refetch={status.refetch} />}
      </Tabs>
    </>
  )
}

const TARGET_P95_MS = 10
const TARGET_P99_MS = 25

function Overview({ status: s }: { status: HubStatus | undefined }) {
  const integrations = useIntegrations()
  const integ = (type: string) => s?.integrations.find((i) => i.type === type)
  const detail = (type: string) => integrations.data?.find((i) => i.type === type)?.detail
  const ble = integ('ble_hid')
  const ha = integ('home_assistant')
  const ev = s?.remote.last_event
  return (
    <div className="grid gap-4 lg:grid-cols-3">
      <div className="grid gap-4 sm:grid-cols-2 lg:col-span-3 lg:grid-cols-4">
        <StatusTile
          icon={Radio}
          title="Remote"
          state={!s ? null : s.remote.paired ? (s.remote.last_packet_age_ms != null ? 'connected' : 'connecting') : 'not_configured'}
          value={s ? (s.remote.paired ? (s.remote.held?.length ? `Holding ${s.remote.held.map(buttonLabel).join(' + ')}` : 'Paired') : 'Not paired') : null}
          detail={
            !s ? null : !s.remote.paired ? (
              <Link href="/settings/remote" className="text-primary underline-offset-4 hover:underline">
                Pair the remote
              </Link>
            ) : ev ? (
              `${buttonLabel(ev.button)} ${EVENT_VERB[ev.kind] ?? ev.kind} · ${agoMs(ev.age_ms)}`
            ) : (
              'No presses yet'
            )
          }
        />
        <StatusTile icon={Bluetooth} title="Bluetooth" state={ble?.state ?? null} value={ble ? healthLabel(ble.state) : s ? 'Not available' : null} detail={detail('ble_hid') ?? ''} />
        <StatusTile icon={Home} title="Home Assistant" state={ha?.state ?? null} value={ha ? healthLabel(ha.state) : s ? 'Not available' : null} detail={detail('home_assistant') ?? ''} />
        <StatusTile
          icon={Cable}
          title="Ethernet"
          state={!s ? null : s.network.link === 'up' ? 'connected' : 'error'}
          value={s ? (s.network.ip ?? (s.network.link === 'up' ? 'No address' : 'Not connected')) : null}
          detail={s?.network.hostname ? `${s.network.hostname}.local` : null}
        />
      </div>
      <ErrorsCard status={s} className="lg:col-span-2" />
      <LatencyCard stats={s ? (s.diagnostics.dispatch_latency ?? null) : undefined} />
    </div>
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
    <figure className="grid gap-1" aria-hidden>
      <div className="relative h-6">
        <div className="absolute inset-x-0 top-2.5 h-1.5 overflow-hidden rounded-full bg-muted">
          <div className="absolute inset-y-0 left-0 bg-success/30" style={{ width: pos(TARGET_P95_MS) }} />
          <div className="absolute inset-y-0 bg-warning/30" style={{ left: pos(TARGET_P95_MS), width: `calc(${pos(TARGET_P99_MS)} - ${pos(TARGET_P95_MS)})` }} />
          <div className="absolute inset-y-0 right-0 bg-destructive/25" style={{ left: pos(TARGET_P99_MS) }} />
        </div>
        {marks.map(([k, v, c]) => (
          <span key={k} title={`${k} ${v} ms`} className={cn('absolute top-1 size-3.5 -translate-x-1/2 rounded-full ring-2 ring-card', c)} style={{ left: pos(v) }} />
        ))}
      </div>
      <div className="relative h-4 text-[10px] text-muted-foreground tabular-nums">
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
        <CardTitle>Response time</CardTitle>
        {stats && <Badge variant={within ? 'success' : 'destructive'}>{within ? 'Good' : 'Slow'}</Badge>}
      </CardHeader>
      <CardContent className="grid gap-4">
        <dl className="grid grid-cols-3 gap-2">
          {p('p50', stats?.p50_ms)}
          {p('p95', stats?.p95_ms, TARGET_P95_MS)}
          {p('p99', stats?.p99_ms, TARGET_P99_MS)}
        </dl>
        {stats ? <LatencyMeter stats={stats} /> : stats === null ? <p className="text-sm text-muted-foreground">No presses yet.</p> : <Skeleton className="h-10" />}
      </CardContent>
    </Card>
  )
}

function ErrorsCard({ status, className }: { status: HubStatus | undefined; className?: string }) {
  const errs = status?.diagnostics.last_errors ?? []
  return (
    <Card className={className}>
      <CardHeader>
        <CardTitle>Recent errors</CardTitle>
        {errs.length > 0 && (
          <Link href="/status/diagnostics" className="text-sm text-primary underline-offset-4 hover:underline">
            See all
          </Link>
        )}
      </CardHeader>
      <CardContent>
        {!status ? (
          <Skeleton className="h-16" />
        ) : errs.length === 0 ? (
          <p className="text-sm text-muted-foreground">No errors.</p>
        ) : (
          <ul className="divide-y">
            {errs.slice(0, 6).map((e, i) => (
              <li key={i} className="flex gap-3 py-2.5 text-sm first:pt-0 last:pb-0">
                <AlertTriangle className="mt-0.5 size-4 shrink-0 text-warning" aria-hidden />
                <div className="min-w-0 flex-1">
                  <p className="break-words">{e.message}</p>
                  {e.at && <p className="text-xs text-muted-foreground">{timeAgo(e.at)}</p>}
                </div>
              </li>
            ))}
          </ul>
        )}
      </CardContent>
    </Card>
  )
}

function Diagnostics({ status, refetch }: { status: HubStatus | undefined; refetch: () => void }) {
  const d = status?.diagnostics
  const r = status?.remote
  const system: [string, ReactNode][] = []
  if (status) {
    system.push(['Firmware', status.hubbo.build ?? status.hubbo.version], ['Board', status.hubbo.board])
    if (d?.heap_free !== undefined) system.push(['Heap free', `${bytes(d.heap_free)}${d.heap_min_free !== undefined ? ` (min ${bytes(d.heap_min_free)})` : ''}`])
    if (d?.psram_free !== undefined) system.push(['PSRAM free', bytes(d.psram_free)])
    system.push(['Queue overflows', <span className={d?.queue_overflows ? 'text-destructive' : undefined}>{d?.queue_overflows ?? 0}</span>])
  }
  return (
    <div className="grid gap-4">
      <Section title="Errors">
        {!d ? (
          <Skeleton className="h-20" />
        ) : !d.last_errors?.length ? (
          <p className="text-sm text-muted-foreground">No errors.</p>
        ) : (
          <pre tabIndex={0} aria-label="Errors" className="max-h-96 overflow-auto rounded-lg bg-muted p-3 font-mono text-xs leading-relaxed outline-none focus-visible:ring-2 focus-visible:ring-ring">
            {d.last_errors.map((e) => `${e.at ?? '-'}  ${(e.source ?? '').padEnd(10)} ${e.code.padEnd(12)} ${e.message}`).join('\n')}
          </pre>
        )}
      </Section>
      <Section title="System">{status ? <Facts rows={system} /> : <Skeleton className="h-16" />}</Section>
      {r && (
        <Section title="Radio">
          <Facts
            rows={[
              ['Channel', r.channel ?? '–'],
              ['Last packet', agoMs(r.last_packet_age_ms)],
              ['Bad checksums', r.counters?.bad_checksum ?? 0],
              ['Unknown codes', r.counters?.unknown_code ?? 0],
            ]}
          />
        </Section>
      )}
      <Section title="Raw status" description="For bug reports.">
        <div className="mb-3 flex justify-end">
          <Button variant="outline" size="sm" onClick={refetch}>
            <RefreshCw /> Refresh
          </Button>
        </div>
        <pre tabIndex={0} aria-label="Raw status" className="max-h-96 overflow-auto rounded-lg bg-muted p-3 font-mono text-xs outline-none focus-visible:ring-2 focus-visible:ring-ring">{status ? JSON.stringify(status, null, 2) : 'Loading…'}</pre>
      </Section>
    </div>
  )
}
