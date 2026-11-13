import { useState, type FormEvent } from 'react'
import type { IntegrationConfig } from '@/api/types'
import { IntegrationSettings, SaveButton, SaveError, type ConnectionHelp, type SaveSettings } from '@/components/admin/IntegrationSettings'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'

const MQTT_HELP: ConnectionHelp = {
  auth_failed: { variant: 'error', text: 'The broker rejected the username or password.' },
  unreachable: { variant: 'warning', text: 'Can’t reach the broker. Check the address and port.' },
}

/** The MQTT broker Hubbo shows up through in Home Assistant, and its connection. */
export function MqttSettings({ id }: { id: string }) {
  return (
    <IntegrationSettings
      id={id}
      help={MQTT_HELP}
      slow="Still connecting. Check the broker, or come back later."
      testId="mqtt-connection"
      form={(config, saving) => <MqttForm config={config} {...saving} />}
    />
  )
}

/** Broker address, port, username and password. The password is write-only: never read back or shown. */
function MqttForm({ config, save: saveSettings, busy, error }: { config: IntegrationConfig } & SaveSettings) {
  const s = config.settings.type === 'mqtt' ? config.settings : null
  const [host, setHost] = useState(s?.host ?? '')
  const [port, setPort] = useState(String(s?.port ?? 1883))
  const [username, setUsername] = useState(s?.username ?? '')
  const [password, setPassword] = useState('')
  if (!s) return null
  // A new broker clears the saved password unless a new one comes with it (the hub's rule).
  const hostChanged = host.trim() !== s.host
  const keepsPassword = s.has_password && !hostChanged

  // `password`: a new one, null to remove the saved one, or left out to keep it.
  const save = (password?: string | null) =>
    saveSettings({ type: 'mqtt', host: host.trim(), port: Number(port), username, ...(password === undefined ? {} : { password }) })
  const submit = (e: FormEvent) => {
    e.preventDefault()
    void save(password || undefined)
  }

  const at = (field: string) => error?.details.find((d) => d.path === `/settings/${field}`)?.message
  const fieldError = error?.details.some((d) => d.path.startsWith('/settings/'))
  return (
    <form onSubmit={submit} className="grid gap-4">
      <div className="grid grid-cols-[1fr_6rem] gap-3">
        <Field label="Broker" htmlFor="mqtt-host" error={at('host')}>
          <Input id="mqtt-host" spellCheck={false} autoCapitalize="none" placeholder="homeassistant.local" value={host} onChange={(e) => setHost(e.target.value)} maxLength={253} required />
        </Field>
        <Field label="Port" htmlFor="mqtt-port" error={at('port')}>
          <Input id="mqtt-port" type="number" inputMode="numeric" min={1} max={65535} value={port} onChange={(e) => setPort(e.target.value)} required />
        </Field>
      </div>
      <Field label="Username" htmlFor="mqtt-user" error={at('username')}>
        <Input id="mqtt-user" spellCheck={false} autoCapitalize="none" value={username} onChange={(e) => setUsername(e.target.value)} maxLength={64} />
      </Field>
      {hostChanged && s.has_password && <Alert variant="warning" title="Enter the password again for the new broker." />}
      <Field label="Password" htmlFor="mqtt-password" error={at('password')} hint={keepsPassword ? 'Leave blank to keep the saved password.' : undefined}>
        <Input id="mqtt-password" type="password" spellCheck={false} placeholder={keepsPassword ? '••••••••' : undefined} value={password} onChange={(e) => setPassword(e.target.value)} maxLength={512} />
      </Field>
      {error && !fieldError && <SaveError error={error} />}
      <div className="flex flex-wrap items-center gap-3">
        <SaveButton busy={busy} />
        {keepsPassword && (
          <Button type="button" variant="ghost" size="sm" disabled={busy} onClick={() => void save(null)}>
            Remove password
          </Button>
        )}
      </div>
    </form>
  )
}
