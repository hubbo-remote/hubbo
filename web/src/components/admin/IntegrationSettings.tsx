import { Loader2 } from 'lucide-react'
import { Fragment, useEffect, useState, type ReactNode } from 'react'
import { api, ApiError, toApiError } from '@/api/client'
import { getQueryData, invalidate, setQueryData } from '@/api/hooks'
import type { IntegrationConfig, IntegrationConfigUpdate, IntegrationState, Status } from '@/api/types'
import { ErrorState, HealthDot } from '@/components/common'
import { Button } from '@/components/ui/button'
import { Alert } from '@/components/ui/alert'
import { Skeleton } from '@/components/ui/misc'
import { useIntegrationConfig, useStatus } from '@/hooks/data'
import { healthLabel } from '@/lib/health'

/** What a connection state means for the admin, for the states that need words. */
export type ConnectionHelp = Partial<Record<IntegrationState, { variant: 'info' | 'warning' | 'error'; text: string }>>

type Settings = NonNullable<IntegrationConfigUpdate['settings']>

/** What a settings form gets: `save` enables the integration with these settings (false when it failed); `error` is the last failure. */
export type SaveSettings = { save: (settings: Settings) => Promise<boolean>; busy: boolean; error: ApiError | null }

/**
 * A connecting integration's settings (Home Assistant, MQTT): loads its config, then shows the form
 * under its connection line. Saving waits for the connection result. `showState` adds the live
 * state line, for places without the Integrations card header.
 */
export function IntegrationSettings({ id, showState, help, slow, testId, form }: {
  id: string
  showState?: boolean
  help: ConnectionHelp
  /** Said when a save is still connecting after a while. */
  slow: string
  testId: string
  /** The form for a loaded config. It remounts on each new revision, which clears a typed secret. */
  form: (config: IntegrationConfig, saving: SaveSettings) => ReactNode
}) {
  const cfg = useIntegrationConfig(id)
  // The state just before the latest save (and when): its outcome only counts once the state moves on.
  const [saved, setSaved] = useState<{ at: number; before: IntegrationState | undefined } | null>(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<ApiError | null>(null)
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} />
  if (!cfg.data) return <Skeleton className="h-48" />
  const { revision } = cfg.data
  const save = async (settings: Settings) => {
    setBusy(true)
    setError(null)
    try {
      setQueryData(`integration-config/${id}`, await api.configureIntegration(id, { revision, enabled: true, settings }))
      // Before status is re-read, so the cache still holds the pre-save state.
      setSaved({ at: Date.now(), before: getQueryData<Status>('status')?.integrations.find((i) => i.id === id)?.state })
      invalidate('integrations', 'status')
      return true
    } catch (err) {
      setError(toApiError(err))
      return false
    } finally {
      setBusy(false)
    }
  }
  return (
    <div className="grid gap-4">
      <Connection key={saved?.at ?? 0} id={id} saved={saved} showState={showState} help={help} slow={slow} testId={testId} />
      <Fragment key={revision}>{form(cfg.data, { save, busy, error })}</Fragment>
    </div>
  )
}

/** A save's error that no field shows: a revision conflict, or the API's message. */
export function SaveError({ error }: { error: ApiError }) {
  return error.code === 'revision_conflict' ? <Alert variant="error">Changed somewhere else. Reload and try again.</Alert> : <ErrorState error={error} />
}

/** The form's Save button, spinning while it saves. */
export function SaveButton({ busy }: { busy: boolean }) {
  return (
    <Button type="submit" disabled={busy}>
      {busy && <Loader2 className="animate-spin" />}
      Save
    </Button>
  )
}

/** How long a save waits for the connection (or its failure) before saying it's taking a while. */
const WAIT_MS = 20_000
/**
 * The hub applies saved settings on its client's next pass (tens of ms), so a status read right
 * after a save can still show the old connection. Until the state moves off its pre-save value, or
 * this long passes (a reconnect that ends where it started), the result shows as connecting.
 */
const APPLY_GRACE_MS = 2_000

/**
 * Live connection state from /status (polled). After a save (`saved`, remounted per save) it polls
 * faster and reports the outcome: connected, the error, or that it's still trying after a while.
 */
function Connection({ id, saved, showState, help: helpFor, slow, testId }: { id: string; saved: { at: number; before: IntegrationState | undefined } | null; showState?: boolean; help: ConnectionHelp; slow: string; testId: string }) {
  const [timedOut, setTimedOut] = useState(false)
  // Whether a read can be the saved settings' result: the state moved off its pre-save value, or the grace passed.
  const [applied, setApplied] = useState(!saved)
  const [settled, setSettled] = useState(false) // the save's outcome is in: back to the normal poll
  const entry = useStatus(saved && !settled && !timedOut ? 1000 : 3000).data?.integrations.find((i) => i.id === id)
  const state = entry?.state
  if (saved && !applied && state !== undefined && state !== saved.before) setApplied(true)
  useEffect(() => {
    if (!saved) return
    const grace = setTimeout(() => setApplied(true), APPLY_GRACE_MS)
    const wait = setTimeout(() => setTimedOut(true), WAIT_MS)
    return () => (clearTimeout(grace), clearTimeout(wait))
  }, [saved])
  if (!state) return null
  // Right after a save, a read that may predate it counts as still connecting.
  const settling = !applied || state === 'connecting' || state === 'authenticating'
  if (saved && !settling && !settled) setSettled(true)
  let help = helpFor[state]
  if (saved && settling) help = timedOut ? { variant: 'warning', text: slow } : { variant: 'info', text: 'Saved. Connecting…' }
  const shown = applied ? state : 'connecting'
  return (
    <div className="grid gap-2 empty:hidden" data-testid={testId}>
      {showState && (
        <p className="flex items-center gap-2 text-sm">
          <HealthDot state={shown} /> {healthLabel(shown, entry.type)}
          {saved && settling && !timedOut && <Loader2 className="size-4 animate-spin text-muted-foreground" aria-hidden />}
        </p>
      )}
      {saved && applied && state === 'connected' ? <Alert variant="success">Saved and connected.</Alert> : help && <Alert variant={help.variant}>{help.text}</Alert>}
    </div>
  )
}
