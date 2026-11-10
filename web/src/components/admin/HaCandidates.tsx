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
          Hubbo can only list entities while it’s connected to Home Assistant. Check its state under Integrations. ({e.message})
        </Alert>
      )
    return <ErrorState error={error} retry={start} what="Browsing Home Assistant entities" />
  }
  if (!disc.data) return <div className="grid gap-2">{[0, 1, 2].map((i) => <Skeleton key={i} className="h-12" />)}</div>

  if (state === 'running' || (state === 'idle' && busy))
    return (
      <div className="flex flex-wrap items-center gap-3 rounded-lg border px-3 py-4 text-sm" aria-live="polite">
        <Loader2 className="size-4 animate-spin text-primary" /> Reading entities and services from Home Assistant…
        <Button size="sm" variant="outline" className="ml-auto" onClick={cancel}>
          Cancel
        </Button>
      </div>
    )
  if (state === 'idle')
    return (
      <div className="grid justify-items-start gap-2 text-sm text-muted-foreground">
        <p>Discovery was cancelled.</p>
        <Button size="sm" onClick={start} disabled={busy}>
          <Search /> Discover entities
        </Button>
      </div>
    )
  if (state === 'failed')
    return (
      <Alert variant="error" title="Discovery failed" action={<Button size="sm" variant="outline" onClick={start} disabled={busy}>Try again</Button>}>
        {disc.data.error ?? 'Home Assistant didn’t answer.'}
      </Alert>
    )

  const list = disc.data.candidates ?? []
  const truncated = disc.data.truncated ?? 0
  return (
    <div className="grid gap-3">
      {truncated > 0 && <Alert variant="warning">{truncated} entities weren’t loaded (more than 2048 supported entities).</Alert>}
      <div className="flex gap-2">
        <div className="relative flex-1">
          <Search className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-muted-foreground" />
          <Input placeholder="Search entities" aria-label="Search Home Assistant entities" value={q} onChange={(e) => setQ(e.target.value)} className="pl-9" maxLength={48} autoFocus />
        </div>
        <Button variant="outline" size="icon" onClick={start} disabled={busy} aria-label="Discover again" title="Discover again">
          <RefreshCw className={busy ? 'animate-spin' : undefined} />
        </Button>
      </div>
      <ul className="grid max-h-80 gap-1 overflow-y-auto" aria-label="Discovered entities">
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
      <p className="text-xs text-muted-foreground">
        {disc.data.total !== undefined && `${disc.data.total} entities found.`} {disc.data.next_cursor && 'Showing the first matches; search to narrow it down.'}
      </p>
    </div>
  )
}

/** What Hubbo would offer for an entity (from its loaded details): built-in commands, its integration's services, and what was left out. */
export function EntityPreview({ c }: { c: EntityDetails }) {
  const ids = c.command_ids ?? []
  const builtIn = ids.filter((id) => id.startsWith(`${c.domain}.`))
  const services = ids.filter((id) => !builtIn.includes(id))
  const group = (title: string, items: string[]) =>
    items.length > 0 && (
      <div className="grid gap-1">
        <p className="text-xs font-medium text-muted-foreground">{title}</p>
        <ul className="flex flex-wrap gap-1">
          {items.map((id) => (
            <li key={id} className="rounded bg-muted px-1.5 py-0.5 font-mono text-xs">
              {id}
            </li>
          ))}
        </ul>
      </div>
    )
  return (
    <section aria-label="Commands this device will have" className="grid max-h-60 gap-3 overflow-y-auto rounded-lg border px-3 py-2">
      {c.platform && (
        <p className="text-xs text-muted-foreground">
          Home Assistant integration: <span className="font-mono text-foreground">{c.platform}</span>
        </p>
      )}
      {!c.platform && <p className="text-xs text-muted-foreground">Defined in Home Assistant’s configuration (YAML), so there are no integration services.</p>}
      {ids.length === 0 && <p className="text-sm text-muted-foreground">Hubbo offers no commands for this entity.</p>}
      {group('Built-in commands', builtIn)}
      {group(c.platform ? `From the ${c.platform} integration` : 'From its Home Assistant integration', services)}
      {(c.skipped_services?.length ?? 0) > 0 && (
        <p className="text-xs text-muted-foreground">
          Not offered (a field Hubbo can’t type, more than 4 fields, or no entity target): <span className="font-mono">{c.skipped_services!.join(', ')}</span>
        </p>
      )}
      {(c.dropped_options ?? 0) > 0 && <p className="text-xs text-muted-foreground">{c.dropped_options} list options were left out (too long or too many); the commands still work with the rest.</p>}
      <p className="text-xs text-muted-foreground">Parameters and their ranges show on the device page once it’s registered.</p>
    </section>
  )
}
