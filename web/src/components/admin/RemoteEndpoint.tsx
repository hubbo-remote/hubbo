import { Loader2 } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { api, toApiError, type ApiError } from '@/api/client'
import { invalidate, setQueryData, useQuery } from '@/api/hooks'
import { REMOTE_CHANNEL_MAX } from '@/api/constants'
import type { RemoteConfig } from '@/api/types'
import { ErrorState } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { checkAddress, formatAddress, parseChannel } from '@/lib/remote'
import { PressCheck } from './PressCheck'

const useRemote = () => useQuery('remote', () => api.remote(), { pollMs: 5000 })

/** The Hub endpoint Hubbo listens on, prefilled with the stored one. The radio's state shows only when it's broken. */
export function RemoteEndpoint() {
  const remote = useRemote()
  if (remote.error) return <ErrorState error={remote.error} retry={remote.refetch} />
  if (!remote.data) return <Skeleton className="h-40" />
  return (
    <div className="grid gap-4">
      {remote.data.radio === 'not_ready' && <Alert variant="error">The remote’s radio isn’t working. Try restarting Hubbo.</Alert>}
      <EndpointForm current={remote.data} />
    </div>
  )
}

function EndpointForm({ current }: { current: RemoteConfig }) {
  // null = not edited: show the stored endpoint, so the fields always say what the hub uses.
  const [editChannel, setChannel] = useState<string | null>(null)
  const [editAddress, setAddress] = useState<string | null>(null)
  const channel = editChannel ?? (current.endpoint ? String(current.endpoint.channel) : '')
  const address = editAddress ?? (current.endpoint ? formatAddress(current.endpoint.address) : '')
  const [touched, setTouched] = useState(false) // errors show from the first submit on (ShowErrors)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<ApiError | null>(null)
  const [savedAt, setSavedAt] = useState<number | null>(null)
  const ch = parseChannel(channel)
  const checked = checkAddress(address)
  const addr = 'address' in checked ? checked.address : null
  const chError = channel !== '' && ch === null ? `A whole number from 0 to ${REMOTE_CHANNEL_MAX}` : undefined
  const addrError = address !== '' && 'error' in checked ? checked.error : undefined
  const scanning = current.radio === 'scanning'

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setTouched(true)
    if (ch === null || addr === null) return
    setBusy(true)
    setError(null)
    setSavedAt(null)
    try {
      setQueryData('remote', await api.setRemoteEndpoint({ channel: ch, address: addr }))
      invalidate('status')
      setChannel(null) // back to showing the stored endpoint, now the saved one
      setAddress(null)
      setTouched(false)
      setSavedAt(Date.now())
    } catch (err) {
      setError(toApiError(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <ShowErrors.Provider value={touched}>
      <form onSubmit={submit} className="grid gap-3" noValidate>
        <div className="grid gap-3 sm:grid-cols-[8rem_1fr]">
          <Field label="Channel" htmlFor="rf-channel" error={chError ?? error?.details.find((d) => d.path === '/channel')?.message}>
            <Input id="rf-channel" inputMode="numeric" placeholder={String(current.endpoint?.channel ?? 74)} value={channel} onChange={(e) => setChannel(e.target.value)} maxLength={3} />
          </Field>
          <Field label="Address" htmlFor="rf-address" error={addrError ?? error?.details.find((d) => d.path === '/address')?.message}>
            <Input id="rf-address" className="font-mono" spellCheck={false} autoComplete="off" placeholder={current.endpoint ? formatAddress(current.endpoint.address) : 'FC:5C:97:55:0F'} value={address} onChange={(e) => setAddress(e.target.value)} maxLength={20} />
          </Field>
        </div>
        {error && !error.details.some((d) => d.path === '/channel' || d.path === '/address') && <ErrorState error={error} />}
        {scanning && <Alert variant="info">Stop the scan to enter these by hand.</Alert>}
        {savedAt !== null && current.endpoint && (
          <>
            <Alert variant="success">Saved.</Alert>
            <PressCheck key={savedAt} />
          </>
        )}
        <div>
          <Button type="submit" variant="outline" disabled={busy || scanning || !channel.trim() || !address.trim()}>
            {busy && <Loader2 className="animate-spin" />} Save
          </Button>
        </div>
      </form>
    </ShowErrors.Provider>
  )
}
