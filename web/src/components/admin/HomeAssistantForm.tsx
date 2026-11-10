import { Loader2 } from 'lucide-react'
import { useEffect, useState, type FormEvent } from 'react'
import { api, ApiError, toApiError } from '@/api/client'
import { getQueryData, invalidate, setQueryData } from '@/api/hooks'
import type { IntegrationConfig, IntegrationState, Status } from '@/api/types'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { ErrorState, HealthDot } from '@/components/common'
import { useIntegrationConfig, useStatus } from '@/hooks/data'
import { healthLabel } from '@/lib/health'

/** Loads the integration's settings, then shows the form. Saving waits for the connection result. */
export function HomeAssistantSettings({ id, onSaved }: { id: string; onSaved?: () => void }) {
  const cfg = useIntegrationConfig(id)
  // The state just before the latest save (and when): its outcome only counts once the state moves on.
  const [saved, setSaved] = useState<{ at: number; before: IntegrationState | undefined } | null>(null)
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} what="Home Assistant settings" />
  if (!cfg.data) return <Skeleton className="h-48" />
  return (
    <div className="grid gap-4">
      <HaConnection key={saved?.at ?? 0} id={id} saved={saved} />
      <HomeAssistantForm
        key={cfg.data.revision}
        config={cfg.data}
        onSaved={() => {
          // Called before status is re-read, so the cache still holds the pre-save state.
          setSaved({ at: Date.now(), before: getQueryData<Status>('status')?.integrations.find((i) => i.id === id)?.state })
          onSaved?.()
        }}
      />
    </div>
  )
}

/** How long a save waits for HA to connect (or fail) before saying it's taking a while. */
const HA_WAIT_MS = 20_000
/**
 * The hub's HA task applies saved settings on its next pass (tens of ms), so a status read right
 * after a save can still show the old connection. Until the state moves off its pre-save value, or
 * this long passes (a reconnect that ends where it started), the result shows as connecting.
 */
const HA_APPLY_GRACE_MS = 2_000

/** What each HA client state means for the admin (design §1). Only the states that need words. */
const CONN_HELP: Partial<Record<IntegrationState, { variant: 'info' | 'warning' | 'error'; text: string }>> = {
  not_configured: { variant: 'info', text: 'Enter the URL and a long-lived access token to connect.' },
  connecting: { variant: 'info', text: 'Connecting to Home Assistant…' },
  authenticating: { variant: 'info', text: 'Connected; signing in with the token…' },
  auth_failed: { variant: 'error', text: 'Home Assistant rejected the token. Hubbo won’t retry until you save a new token (or URL).' },
  unreachable: { variant: 'warning', text: 'Home Assistant can’t be reached. Hubbo keeps retrying (every 1 s, backing off to once a minute). Check the URL and that Home Assistant is running.' },
}

/**
 * Live connection state from /status (polled). After a save (`saved`, remounted per save) it polls
 * faster and reports the outcome: connected, the error, or that it's still trying after a while.
 */
function HaConnection({ id, saved }: { id: string; saved: { at: number; before: IntegrationState | undefined } | null }) {
  const [timedOut, setTimedOut] = useState(false)
  // Whether a read can be the saved settings' result: the state moved off its pre-save value, or the grace passed.
  const [applied, setApplied] = useState(!saved)
  const [settled, setSettled] = useState(false) // the save's outcome is in: back to the normal poll
  const state = useStatus(saved && !settled && !timedOut ? 1000 : 3000).data?.integrations.find((i) => i.id === id)?.state
  if (saved && !applied && state !== undefined && state !== saved.before) setApplied(true)
  useEffect(() => {
    if (!saved) return
    const grace = setTimeout(() => setApplied(true), HA_APPLY_GRACE_MS)
    const wait = setTimeout(() => setTimedOut(true), HA_WAIT_MS)
    return () => (clearTimeout(grace), clearTimeout(wait))
  }, [saved])
  if (!state) return null
  // Right after a save, a read that may predate it counts as still connecting.
  const settling = !applied || state === 'connecting' || state === 'authenticating'
  if (saved && !settling && !settled) setSettled(true)
  let help = CONN_HELP[state]
  if (saved && settling)
    help = timedOut
      ? { variant: 'warning', text: `Still not connected after ${HA_WAIT_MS / 1000} seconds. Hubbo keeps trying in the background; check the URL and that Home Assistant is running, or carry on and come back to it later.` }
      : { variant: 'info', text: 'Saved. Connecting to Home Assistant…' }
  return (
    <div className="grid gap-2" data-testid="ha-connection">
      <p className="flex items-center gap-2 text-sm">
        <HealthDot state={applied ? state : 'connecting'} /> <span className="font-medium">Connection:</span> {healthLabel(applied ? state : 'connecting')}
        {saved && settling && !timedOut && <Loader2 className="size-4 animate-spin text-muted-foreground" aria-hidden />}
      </p>
      {saved && applied && state === 'connected' ? <Alert variant="success">Saved and connected to Home Assistant.</Alert> : help && <Alert variant={help.variant}>{help.text}</Alert>}
    </div>
  )
}

/** HA URL + long-lived token. The token is write-only: never read back or shown. */
function HomeAssistantForm({ config, onSaved }: { config: IntegrationConfig; onSaved?: () => void }) {
  const s = config.settings.type === 'home_assistant' ? config.settings : null
  const [url, setUrl] = useState(s?.url ?? '')
  const [token, setToken] = useState('')
  const [verify, setVerify] = useState(s?.verify_tls ?? true)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<ApiError | null>(null)
  if (!s) return null
  const urlChanged = url.replace(/\/$/, '') !== s.url.replace(/\/$/, '')
  const keepsToken = s.has_access_token && !urlChanged

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true)
    setError(null)
    try {
      const saved = await api.configureIntegration(config.id, {
        revision: config.revision,
        enabled: true,
        settings: { type: 'home_assistant', url, verify_tls: verify, ...(token ? { access_token: token } : {}) },
      })
      setToken('')
      setQueryData(`integration-config/${config.id}`, saved)
      onSaved?.() // first: it notes the pre-save state, which the re-read below replaces
      invalidate('integrations', 'status')
    } catch (err) {
      setError(toApiError(err))
    } finally {
      setBusy(false)
    }
  }

  const urlError = error?.details.find((d) => d.path === '/settings/url')?.message
  return (
    <form onSubmit={submit} className="grid gap-4">
      <Field label="Home Assistant URL" htmlFor="ha-url" hint="Base URL only. Use https:// if Home Assistant has a certificate." error={urlError}>
        <Input id="ha-url" type="url" inputMode="url" placeholder="https://homeassistant.local:8123" value={url} onChange={(e) => setUrl(e.target.value)} maxLength={200} required />
      </Field>
      {url.startsWith('https://') && (
        <div className="flex items-center justify-between gap-3 rounded-md border px-3 py-2">
          <label htmlFor="ha-tls" className="text-sm">
            <span className="font-medium">Verify certificate</span>
            <span className="block text-xs text-muted-foreground">Off still encrypts, but doesn’t check who answers. Only for a self-signed Home Assistant on a trusted LAN.</span>
          </label>
          <Switch id="ha-tls" checked={verify} onCheckedChange={setVerify} />
        </div>
      )}
      {urlChanged && s.has_access_token && (
        <Alert variant="warning" title="Changing the URL clears the stored token">
          The hub forgets the token when the URL changes, so enter it again for the new address.
        </Alert>
      )}
      <Field
        label="Long-lived access token"
        htmlFor="ha-token"
        hint={keepsToken ? 'A token is stored on the hub. Leave blank to keep it.' : urlChanged && s.has_access_token ? 'Enter the token for the new address.' : 'Create one in Home Assistant under your profile → Security.'}
      >
        <Input id="ha-token" type="password" autoComplete="off" spellCheck={false} placeholder={keepsToken ? '•••••••• (stored)' : 'Paste token'} value={token} onChange={(e) => setToken(e.target.value)} maxLength={512} required={!keepsToken} />
      </Field>
      <Alert variant="info">Hubbo only calls services on devices you register, but Home Assistant tokens aren’t sandboxed: anyone holding this token can control everything it can.</Alert>
      {error && !urlError && (error.code === 'revision_conflict' ? <Alert variant="error">These settings changed in another window. Reload and try again.</Alert> : <ErrorState error={error} what="Configuring Home Assistant" />)}
      <div>
        <Button type="submit" disabled={busy}>
          {busy && <Loader2 className="animate-spin" />}
          Save and connect
        </Button>
      </div>
    </form>
  )
}
