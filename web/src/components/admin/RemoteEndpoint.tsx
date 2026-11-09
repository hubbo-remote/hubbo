import { Loader2 } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { api, toApiError, type ApiError } from '@/api/client'
import { setQueryData, useQuery } from '@/api/hooks'
import { REMOTE_CHANNEL_MAX } from '@/api/constants'
import type { RemoteConfig } from '@/api/types'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { checkAddress, formatAddress, parseChannel } from '@/lib/remote'

const RADIO: Record<RemoteConfig['radio'], string> = { not_ready: 'Not working (check the radio module)', idle: 'Idle: no endpoint yet', listening: 'Listening', scanning: 'Scanning for a Hub' }

const useRemote = () => useQuery('remote', () => api.remote(), { pollMs: 5000 })

/** The Hub endpoint Hubbo listens on, and manual entry for a known one. */
export function RemoteEndpoint() {
  const remote = useRemote()
  if (remote.error) return <ErrorState error={remote.error} retry={remote.refetch} what="Remote endpoint" />
  if (!remote.data) return <Skeleton className="h-40" />
  return (
    <div className="grid gap-4">
      <dl className="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1 text-sm" aria-label="Current remote endpoint">
        <dt className="text-muted-foreground">Channel</dt>
        <dd className="font-mono">{remote.data.endpoint?.channel ?? 'not set'}</dd>
        <dt className="text-muted-foreground">Address</dt>
        <dd className="font-mono">{remote.data.endpoint ? formatAddress(remote.data.endpoint.address) : 'not set'}</dd>
        <dt className="text-muted-foreground">Radio</dt>
        <dd>{RADIO[remote.data.radio] ?? remote.data.radio}</dd>
      </dl>
      <EndpointForm current={remote.data} />
    </div>
  )
}

function EndpointForm({ current }: { current: RemoteConfig }) {
  const [channel, setChannel] = useState('')
  const [address, setAddress] = useState('')
  const [touched, setTouched] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<ApiError | null>(null)
  const [saved, setSaved] = useState(false)
  const ch = parseChannel(channel)
  const checked = checkAddress(address)
  const addr = 'address' in checked ? checked.address : null
  const chError = touched && channel !== '' && ch === null ? `A whole number from 0 to ${REMOTE_CHANNEL_MAX}` : undefined
  const addrError = touched && address !== '' && 'error' in checked ? checked.error : undefined
  const scanning = current.radio === 'scanning'

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setTouched(true)
    if (ch === null || addr === null) return
    setBusy(true)
    setError(null)
    setSaved(false)
    try {
      setQueryData('remote', await api.setRemoteEndpoint({ channel: ch, address: addr }))
      setChannel('')
      setAddress('')
      setTouched(false)
      setSaved(true)
    } catch (err) {
      setError(toApiError(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <form onSubmit={submit} className="grid gap-3" noValidate>
      <p className="text-sm text-muted-foreground">Already know your Hub’s radio endpoint (e.g. from the serial console)? Enter it instead of scanning.</p>
      <div className="grid gap-3 sm:grid-cols-[8rem_1fr]">
        <Field label="Channel" htmlFor="rf-channel" error={chError ?? error?.details.find((d) => d.path === '/channel')?.message}>
          <Input id="rf-channel" inputMode="numeric" placeholder={String(current.endpoint?.channel ?? 74)} value={channel} onChange={(e) => setChannel(e.target.value)} onBlur={() => setTouched(true)} maxLength={3} />
        </Field>
        <Field label="Address" htmlFor="rf-address" error={addrError ?? error?.details.find((d) => d.path === '/address')?.message}>
          <Input id="rf-address" className="font-mono" spellCheck={false} autoComplete="off" placeholder={current.endpoint ? formatAddress(current.endpoint.address) : 'FC:5C:97:55:0F'} value={address} onChange={(e) => setAddress(e.target.value)} onBlur={() => setTouched(true)} maxLength={20} />
        </Field>
      </div>
      {error && !error.details.some((d) => d.path === '/channel' || d.path === '/address') && <ErrorState error={error} what="Setting the remote endpoint" />}
      {scanning && <Alert variant="info">A scan is running. Cancel it to enter an endpoint by hand.</Alert>}
      {saved && <Alert variant="success">Listening on the new endpoint. Press a remote button to check.</Alert>}
      <div>
        <Button type="submit" variant="outline" disabled={busy || scanning || ch === null || addr === null}>
          {busy && <Loader2 className="animate-spin" />} Use this endpoint
        </Button>
      </div>
    </form>
  )
}
