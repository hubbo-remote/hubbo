import { Check, Loader2, RefreshCw, Search } from 'lucide-react'
import { useCallback, useEffect, useRef, useState } from 'react'
import { api, toApiError } from '@/api/client'
import { getQueryData, setQueryData, useQuery } from '@/api/hooks'
import type { DiscoverCandidate, DiscoverStatus } from '@/api/types'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import type { EntityDetails } from '@/lib/ha'

/** Why a candidate can't be picked, or null if it can. */
type Blocker = (c: DiscoverCandidate) => string | null
const defaultBlocker: Blocker = (c) => (c.registered_device_id ? 'Added' : !c.supported ? 'Not supported' : null)

/**
 * Home Assistant entities, from an on-demand discovery on the hub: start it, poll
 * until it's done (it reads HA's states, services and entity registry), then search.
 */
export function HaCandidates({ integration, onPick, blocker = defaultBlocker }: { integration: string; onPick: (c: DiscoverCandidate) => void; blocker?: Blocker }) {
  const [q, setQ] = useState('')
  const [query, setQuery] = useState('')
  useEffect(() => {
    const t = setTimeout(() => setQuery(q.trim()), 250)
    return () => clearTimeout(t)
  }, [q])
  const key = `discover/${integration}/${query}`
  const running = getQueryData<DiscoverStatus>(key)?.state === 'running'
  // keepPrevious: the search box and the last results stay while a new search loads.
  const disc = useQuery(key, () => api.discovery(integration, query ? { q: query } : {}), { pollMs: running ? 1000 : undefined, keepPrevious: true })
  const [startError, setStartError] = useState<unknown>(null)
  const [busy, setBusy] = useState(false)
  const autoStarted = useRef(false)
  const state = disc.data?.state
  const { refetch } = disc

  const start = useCallback(async () => {
    setBusy(true)
    setStartError(null)
    try {
      setQueryData(key, await api.discoverStart(integration))
    } catch (e) {
      setStartError(e)
    } finally {
      setBusy(false)
    }
  }, [integration, key])
  const cancel = async () => {
    await api.discoverCancel(integration).catch(setStartError)
    await refetch()
  }

  // Nothing discovered since boot: start once, without making the admin click.
  useEffect(() => {
    if (state === 'idle' && !autoStarted.current) {
      autoStarted.current = true
      void start()
    }
  }, [state, start])

  const error = startError ?? disc.error
  if (error) {
    const e = toApiError(error)
    if (e.code === 'integration_unavailable')
      return (
        <Alert variant="error" title="Home Assistant isn’t connected" action={<Button size="sm" variant="outline" onClick={start}>Retry</Button>}>
          Connect it under Integrations, then try again.
        </Alert>
      )
    return <ErrorState error={error} retry={start} />
  }
  if (!disc.data) return <div className="grid gap-2">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-12" />)}</div>

  if (state === 'running' || (state === 'idle' && busy))
    return (
      <div className="flex flex-wrap items-center gap-3 rounded-lg border px-3 py-4 text-sm" aria-live="polite">
        <Loader2 className="size-4 animate-spin text-primary" /> Looking for devices…
        <Button size="sm" variant="outline" className="ml-auto" onClick={cancel}>
          Cancel
        </Button>
      </div>
    )
  if (state === 'idle')
    return (
      <div className="grid justify-items-start gap-2 text-sm text-muted-foreground">
        <p>Stopped.</p>
        <Button size="sm" onClick={start} disabled={busy}>
          <Search /> Look again
        </Button>
      </div>
    )
  if (state === 'failed')
    return (
      <Alert variant="error" title="Couldn’t load devices" action={<Button size="sm" variant="outline" onClick={start} disabled={busy}>Try again</Button>}>
        {disc.data.error ?? 'Home Assistant didn’t answer.'}
      </Alert>
    )

  const list = disc.data.candidates ?? []
  const truncated = disc.data.truncated ?? 0
  return (
    <div className="grid gap-3">
      {truncated > 0 && <Alert variant="warning">{truncated} more weren’t loaded.</Alert>}
      <div className="flex gap-2">
        <div className="relative flex-1">
          <Search className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-muted-foreground" />
          <Input placeholder="Search" aria-label="Search devices" value={q} onChange={(e) => setQ(e.target.value)} className="pl-9" maxLength={48} autoFocus />
        </div>
        <Button variant="outline" size="icon" onClick={start} disabled={busy} aria-label="Refresh" title="Refresh">
          <RefreshCw className={busy ? 'animate-spin' : undefined} />
        </Button>
      </div>
      <ul className="grid max-h-80 gap-1 overflow-y-auto" aria-label="Devices found">
        {list.map((c) => {
          const blocked = blocker(c)
          return (
            <li key={c.provider.entity_id}>
              <button
                disabled={!!blocked}
                onClick={() => onPick(c)}
                className="flex w-full cursor-pointer items-center gap-3 rounded-md px-3 py-2 text-left outline-none hover:bg-accent focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-inset disabled:cursor-default disabled:opacity-60 disabled:hover:bg-transparent"
              >
                <div className="min-w-0 flex-1">
                  <p className="truncate text-sm font-medium">{c.name}</p>
                  <p className="truncate font-mono text-xs text-muted-foreground">
                    {c.provider.entity_id}
                    {c.platform && ` · ${c.platform}`}
                    {c.state && ` · ${c.state}`}
                  </p>
                </div>
                {blocked === 'Added' ? (
                  <span className="inline-flex items-center gap-1 text-xs text-muted-foreground">
                    <Check className="size-3" /> Added
                  </span>
                ) : (
                  // No command count: a candidate previews only its domain's commands; the full list loads once it's picked.
                  blocked && <span className="text-xs text-muted-foreground">{blocked}</span>
                )}
              </button>
            </li>
          )
        })}
        {list.length === 0 && <li className="px-3 py-6 text-center text-sm text-muted-foreground">Nothing matches.</li>}
      </ul>
      {disc.data.next_cursor && <p className="text-xs text-muted-foreground">Search to see more.</p>}
    </div>
  )
}

/** The commands an entity would have (from its loaded details). */
export function EntityPreview({ c }: { c: EntityDetails }) {
  const ids = c.command_ids ?? []
  return (
    <section aria-label="Commands" className="grid max-h-60 gap-2 overflow-y-auto rounded-lg border px-3 py-2">
      <p className="text-xs font-medium text-muted-foreground">Commands</p>
      {ids.length === 0 ? (
        <p className="text-sm text-muted-foreground">None</p>
      ) : (
        <ul className="flex flex-wrap gap-1">
          {ids.map((id) => (
            <li key={id} className="rounded bg-muted px-1.5 py-0.5 font-mono text-xs">
              {id}
            </li>
          ))}
        </ul>
      )}
    </section>
  )
}
