import { useState, type FormEvent } from 'react'
import type { IntegrationConfig } from '@/api/types'
import { IntegrationSettings, SaveActions, SaveError, type ConnectionHelp, type SaveSettings } from '@/components/admin/IntegrationSettings'
import { Alert } from '@/components/ui/alert'
import { Field, Input } from '@/components/ui/input'
import { Switch } from '@/components/ui/switch'
import { usePendingFormSave } from '@/hooks/stepSave'

/** What each HA client state means for the admin (design §1). Only the states that need words. */
const HA_HELP: ConnectionHelp = {
  auth_failed: { variant: 'error', text: 'The token was rejected. Enter a new one.' },
  unreachable: { variant: 'warning', text: 'Can’t reach Home Assistant. Check the URL.' },
}

/** Home Assistant's settings and connection; `showState` adds the live state line (setup). */
export function HomeAssistantSettings({ id, showState }: { id: string; showState?: boolean }) {
  return (
    <IntegrationSettings
      id={id}
      showState={showState}
      help={HA_HELP}
      slow="Still connecting. Check the URL, or come back later."
      testId="ha-connection"
      form={(config, saving) => <HomeAssistantForm config={config} {...saving} />}
    />
  )
}

/** HA URL + long-lived token. The token is write-only: never read back or shown. */
function HomeAssistantForm({ config, save, busy, error }: { config: IntegrationConfig } & SaveSettings) {
  const s = config.settings.type === 'home_assistant' ? config.settings : null
  const [url, setUrl] = useState(s?.url ?? '')
  const [token, setToken] = useState('')
  const [verify, setVerify] = useState(s?.verify_tls ?? true)
  const urlChanged = !!s && url.replace(/\/$/, '') !== s.url.replace(/\/$/, '')
  // `access_token`: a new one, null to remove the saved one, or left out to keep it (like MQTT's password).
  const send = (access_token?: string | null) =>
    save({ type: 'home_assistant', url, verify_tls: verify, ...(access_token === undefined ? {} : { access_token }) })
  // In setup, Continue saves what's typed (without waiting for the connection).
  const dirty = !!s && (urlChanged || !!token || verify !== s.verify_tls)
  const form = usePendingFormSave(dirty, () => send(token || undefined))
  if (!s) return null
  const keepsToken = s.has_access_token && !urlChanged

  const submit = (e: FormEvent) => {
    e.preventDefault()
    void send(token || undefined)
  }

  const urlError = error?.details.find((d) => d.path === '/settings/url')?.message
  return (
    <form ref={form} onSubmit={submit} className="grid gap-4">
      <Field label="URL" htmlFor="ha-url" error={urlError}>
        <Input id="ha-url" type="url" inputMode="url" placeholder="http://homeassistant.local:8123" value={url} onChange={(e) => setUrl(e.target.value)} maxLength={200} required />
      </Field>
      {url.startsWith('https://') && (
        <div className="flex items-center justify-between gap-3 rounded-md border px-3 py-2">
          <label htmlFor="ha-tls" className="text-sm">
            <span className="font-medium">Verify certificate</span>
            <span className="block text-xs text-muted-foreground">Turn off for a self-signed certificate.</span>
          </label>
          <Switch id="ha-tls" checked={verify} onCheckedChange={setVerify} />
        </div>
      )}
      {urlChanged && s.has_access_token && (
        <Alert variant="warning" title="Enter the token again for the new URL." />
      )}
      <Field
        label="Access token"
        htmlFor="ha-token"
        hint={keepsToken ? 'Leave blank to keep the saved token.' : 'Create a long-lived token in your Home Assistant profile.'}
      >
        <Input id="ha-token" type="password" spellCheck={false} placeholder={keepsToken ? '••••••••' : 'Paste token'} value={token} onChange={(e) => setToken(e.target.value)} maxLength={512} required={!keepsToken} />
      </Field>
      {error && !urlError && <SaveError error={error} />}
      <SaveActions busy={busy} remove={keepsToken ? { label: 'Remove token', onRemove: () => void send(null).then((ok) => ok && setToken('')) } : undefined} />
    </form>
  )
}
