import { Loader2, Power, Save } from 'lucide-react'
import { useEffect, useState, type FormEvent } from 'react'
import { api, ApiError, errorMessage } from '@/api/client'
import { useQuery } from '@/api/hooks'
import { ErrorState, Section } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { HUB_NAME_MAX, hubNameProblem, saveHubName, trimHubName } from '@/lib/hub'
import { backFromRestart, followRestart, NOT_BACK } from '@/lib/restart'


/** Settings → Hub: the hub's name, and a restart. */
export function HubSettings() {
  return (
    <div className="grid gap-4">
      <HubName />
      <Restart />
    </div>
  )
}

function HubName() {
  const hub = useQuery('hub', () => api.hub())
  if (hub.error && !hub.data) return <ErrorState error={hub.error} retry={hub.refetch} />
  return <Section title="Name">{hub.data ? <NameForm saved={hub.data.name} taken={hub.data.hostname_conflict ? hub.data.hostname : null} /> : <Skeleton className="h-9 w-72" />}</Section>
}

/** `taken`: the hostname this hub advertises instead, since another hub on the network has the name's. */
function NameForm({ saved, taken }: { saved: string; taken: string | null }) {
  const [name, setName] = useState(saved)
  const [busy, setBusy] = useState(false)
  const [attempted, setAttempted] = useState(false)
  const [serverError, setServerError] = useState<string | null>(null)
  const [renamed, setRenamed] = useState(false)
  const local = hubNameProblem(name)
  const error = serverError ?? (attempted ? local : null)

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setAttempted(true)
    if (local) return
    setBusy(true)
    try {
      const r = await saveHubName(name)
      setName(r.name)
      setRenamed(true)
      toast('Saved', { variant: 'success' })
    } catch (err) {
      const onName = err instanceof ApiError ? err.details.find((d) => d.path === '/name') : undefined
      if (onName) setServerError(onName.message)
      else toastError('Couldn’t save', err)
    } finally {
      setBusy(false)
    }
  }

  return (
    <ShowErrors.Provider value={attempted || !!serverError}>
      <form onSubmit={submit} className="grid max-w-sm gap-3" noValidate>
        <Field label="Name" htmlFor="hub-name" error={error} hint={taken && `Another hub on your network already uses this name, so this one is at ${taken}.local.`}>
          <Input
            id="hub-name"
            value={name}
            maxLength={HUB_NAME_MAX * 2} // the count that matters is characters, checked above
            onChange={(e) => {
              setName(e.target.value)
              setServerError(null)
              setRenamed(false)
            }}
            aria-invalid={!!error}
          />
        </Field>
        <div className="flex flex-wrap items-center gap-3">
          <Button type="submit" disabled={busy || trimHubName(name) === saved}>
            {busy ? <Loader2 className="animate-spin" /> : <Save />} Save
          </Button>
          {renamed && <p className="text-sm text-muted-foreground">Paired devices may show the old name until they reconnect.</p>}
        </div>
      </form>
    </ShowErrors.Provider>
  )
}

function Restart() {
  const [confirm, setConfirm] = useState(false)
  const [state, setState] = useState<'idle' | 'sending' | 'restarting'>('idle')
  const [slow, setSlow] = useState(false)
  const [error, setError] = useState<unknown>(null)

  // Restarting: once the hub is back, the app opens afresh, on the sign-in.
  useEffect(() => (state === 'restarting' ? followRestart(backFromRestart, () => setSlow(true)) : undefined), [state])

  const restart = async () => {
    setState('sending')
    setError(null)
    try {
      await api.restart()
      setState('restarting')
    } catch (e) {
      setState('idle')
      setError(e)
    }
  }
  const close = () => {
    if (state !== 'idle') return
    setConfirm(false)
    setError(null)
  }

  return (
    <Section title="Restart">
      <Button variant="outline" onClick={() => setConfirm(true)}>
        <Power /> Restart…
      </Button>
      <Dialog
        open={confirm}
        onClose={close}
        title="Restart the hub?"
        description="You’ll need to sign in again."
        footer={
          state === 'restarting' ? undefined : (
            <>
              <Button variant="ghost" onClick={close} disabled={state !== 'idle'}>
                Cancel
              </Button>
              <Button onClick={restart} disabled={state !== 'idle'}>
                {state === 'sending' && <Loader2 className="animate-spin" />} Restart
              </Button>
            </>
          )
        }
      >
        {state === 'restarting' ? (
          <p className="inline-flex items-center gap-2 text-sm" role="status">
            <Loader2 className="size-4 animate-spin text-primary" />
            {slow ? NOT_BACK : 'Restarting…'}
          </p>
        ) : (
          error != null && (
            <Alert variant="error" title="Hubbo didn’t restart">
              <p>{error instanceof ApiError && error.code === 'update_in_progress' ? 'An update is running. Try again when it’s done.' : errorMessage(error)}</p>
            </Alert>
          )
        )}
      </Dialog>
    </Section>
  )
}
