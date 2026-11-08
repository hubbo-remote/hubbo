import { Loader2 } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { api, ApiError, toApiError } from '@/api/client'
import { invalidate, setQueryData } from '@/api/hooks'
import type { IntegrationConfig } from '@/api/types'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { useIntegrationConfig } from '@/hooks/data'

/** Loads the integration's settings, then shows the form. */
export function HomeAssistantSettings({ id, onSaved }: { id: string; onSaved?: () => void }) {
  const cfg = useIntegrationConfig(id)
  if (cfg.error) return <ErrorState error={cfg.error} retry={cfg.refetch} />
  if (!cfg.data) return <Skeleton className="h-48" />
  return <HomeAssistantForm key={cfg.data.revision} config={cfg.data} onSaved={onSaved} />
}

/** HA URL + long-lived token. The token is write-only: never read back or shown. */
function HomeAssistantForm({ config, onSaved }: { config: IntegrationConfig; onSaved?: () => void }) {
  const s = config.settings.type === 'home_assistant' ? config.settings : null
  const [url, setUrl] = useState(s?.url ?? '')
  const [verify, setVerify] = useState(s?.verify_tls ?? true)
  const [token, setToken] = useState('')
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
            <span className="block text-xs text-muted-foreground">Turn off only for a self-signed certificate on your own network.</span>
          </label>
          <Switch id="ha-tls" checked={verify} onCheckedChange={setVerify} />
        </div>
      )}
      <Field
        label="Long-lived access token"
        htmlFor="ha-token"
        hint={keepsToken ? 'A token is stored on the hub. Leave blank to keep it.' : urlChanged && s.has_access_token ? 'Changing the URL clears the stored token, so enter it again.' : 'Create one in Home Assistant under your profile → Security.'}
      >
        <Input id="ha-token" type="password" autoComplete="off" spellCheck={false} placeholder={keepsToken ? '•••••••• (stored)' : 'Paste token'} value={token} onChange={(e) => setToken(e.target.value)} maxLength={512} required={!keepsToken} />
      </Field>
      <Alert variant="info">Hubbo only calls services on devices you register, but Home Assistant tokens aren’t sandboxed: anyone holding this token can control everything it can.</Alert>
      {error && !urlError && <Alert variant="error">{error.code === 'revision_conflict' ? 'These settings changed in another window. Reload and try again.' : error.message}</Alert>}
      <div>
        <Button type="submit" disabled={busy}>
          {busy && <Loader2 className="animate-spin" />}
          Save and connect
        </Button>
      </div>
    </form>
  )
}
