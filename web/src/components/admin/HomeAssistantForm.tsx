import { Loader2 } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { api, ApiError, toApiError } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { IntegrationConfig, IntegrationState } from '@/api/types'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { ErrorState, HealthDot } from '@/components/common'
import { useIntegrationConfig, useStatus } from '@/hooks/data'
import { healthLabel } from '@/lib/health'

/** Loads the integration's settings, then shows the form. */
export function HomeAssistantSettings({ id, onSaved }: { id: string; onSaved?: () => void }) {
  const cfg = useIntegrationConfig(id)
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} what="Home Assistant settings" />
  if (!cfg.data) return <Skeleton className="h-48" />
  return (
    <div className="grid gap-4">
      <HaConnection id={id} />
      <HomeAssistantForm key={cfg.data.revision} config={cfg.data} onSaved={onSaved} />
    </div>
  )
}

/** What each HA client state means for the admin (design §1). Only the states that need words. */
const CONN_HELP: Partial<Record<IntegrationState, { variant: 'info' | 'warning' | 'error'; text: string }>> = {
  not_configured: { variant: 'info', text: 'Enter the URL and a long-lived access token to connect.' },
  connecting: { variant: 'info', text: 'Connecting to Home Assistant…' },
  authenticating: { variant: 'info', text: 'Connected; signing in with the token…' },
  auth_failed: { variant: 'error', text: 'Home Assistant rejected the token. Hubbo won’t retry until you save a new token (or URL).' },
  unreachable: { variant: 'warning', text: 'Home Assistant can’t be reached. Hubbo keeps retrying (every 1 s, backing off to once a minute). Check the URL and that Home Assistant is running.' },
}

/** Live connection state from /status (polled). */
function HaConnection({ id }: { id: string }) {
  const state = useStatus().data?.integrations.find((i) => i.id === id)?.state
  if (!state) return null
  const help = CONN_HELP[state]
  return (
    <div className="grid gap-2" data-testid="ha-connection">
      <p className="flex items-center gap-2 text-sm">
        <HealthDot state={state} /> <span className="font-medium">Connection:</span> {healthLabel(state)}
      </p>
      {help && <Alert variant={help.variant}>{help.text}</Alert>}
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
      invalidate('integrations', 'status')
      onSaved?.()
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
