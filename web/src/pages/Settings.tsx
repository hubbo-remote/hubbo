import { Check, Copy, KeyRound, Loader2, Plus, RotateCcw, Trash2 } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useLocation } from 'wouter'
import { api, errorMessage } from '@/api/client'
import { invalidate, invalidateAll, useQuery } from '@/api/hooks'
import type { TokenCreated } from '@/api/types'
import { FactoryReset } from '@/components/admin/FactoryReset'
import { HubSettings } from '@/components/admin/HubSettings'
import { NetworkInfo } from '@/components/admin/NetworkInfo'
import { PairRemote } from '@/components/admin/PairRemote'
import { RemoteEndpoint } from '@/components/admin/RemoteEndpoint'
import { ErrorState, PageHeader, Section, TypedWordField } from '@/components/common'
import { RemoteSvg } from '@/components/remote/RemoteSvg'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { Tabs } from '@/components/ui/tabs'
import { toast, toastError } from '@/components/ui/toast'
import { useStatus } from '@/hooks/data'
import { timeAgo, typedMatches } from '@/lib/utils'
import { NotFound } from './NotFound'

const TABS = [
  { id: 'hub', label: 'Hub' },
  { id: 'account', label: 'Account' },
  { id: 'tokens', label: 'API tokens' },
  { id: 'remote', label: 'Remote' },
  { id: 'network', label: 'Network' },
  { id: 'reset', label: 'Reset' },
]

export default function Settings({ tab = 'hub' }: { tab?: string }) {
  const [, navigate] = useLocation()
  if (!TABS.some((t) => t.id === tab)) return <NotFound />
  return (
    <>
      <PageHeader title="Settings" />
      <Tabs tabs={TABS} value={tab} onChange={(t) => navigate(`/settings/${t}`, { replace: true })}>
        {tab === 'hub' && <HubSettings />}
        {tab === 'account' && <Account />}
        {tab === 'tokens' && <Tokens />}
        {tab === 'remote' && <RemoteTab />}
        {tab === 'network' && <NetworkInfo />}
        {tab === 'reset' && (
          <div className="grid gap-4">
            <ResetConfiguration />
            <FactoryReset />
          </div>
        )}
      </Tabs>
    </>
  )
}

function Account() {
  const [cur, setCur] = useState('')
  const [next, setNext] = useState('')
  const [confirm, setConfirm] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const mismatch = confirm.length > 0 && confirm !== next
  const [attempted, setAttempted] = useState(false) // errors show from the first submit on

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setAttempted(true)
    if (mismatch) return
    setBusy(true)
    setError(null)
    try {
      await api.changePassword(cur, next)
      setCur('')
      setNext('')
      setConfirm('')
      toast('Password changed', { variant: 'success', description: 'Other devices were signed out.' })
    } catch (err) {
      setError(errorMessage(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="grid gap-4">
      <Section title="Password">
        <ShowErrors.Provider value={attempted}>
          <form onSubmit={submit} className="grid max-w-sm gap-4">
            {error && <Alert variant="error">{error}</Alert>}
            <Field label="Current password" htmlFor="cp">
              <Input id="cp" type="password" autoComplete="current-password" value={cur} onChange={(e) => setCur(e.target.value)} required />
            </Field>
            <Field label="New password" htmlFor="np" hint="At least 10 characters.">
              <Input id="np" type="password" autoComplete="new-password" minLength={10} value={next} onChange={(e) => setNext(e.target.value)} required />
            </Field>
            <Field label="Confirm new password" htmlFor="np2" error={mismatch ? 'Passwords don’t match' : null}>
              <Input id="np2" type="password" autoComplete="new-password" value={confirm} onChange={(e) => setConfirm(e.target.value)} aria-invalid={mismatch} required />
            </Field>
            <div>
              <Button type="submit" disabled={busy}>
                {busy && <Loader2 className="animate-spin" />} Change password
              </Button>
            </div>
          </form>
        </ShowErrors.Provider>
      </Section>
      <Section title="Setup">
        <a href="#/setup/network" className="text-sm font-medium text-primary underline-offset-4 hover:underline">
          Run setup again
        </a>
      </Section>
    </div>
  )
}

function Tokens() {
  const tokens = useQuery('tokens', () => api.tokens())
  const [name, setName] = useState('')
  const [created, setCreated] = useState<TokenCreated | null>(null)
  const [copied, setCopied] = useState(false)
  const [busy, setBusy] = useState(false)

  const create = async (e: FormEvent) => {
    e.preventDefault()
    setBusy(true)
    try {
      setCreated(await api.createToken(name))
      setName('')
      setCopied(false)
      invalidate('tokens')
    } catch (err) {
      toastError('Couldn’t create the token', err)
    } finally {
      setBusy(false)
    }
  }

  const revoke = async (id: string) => {
    try {
      await api.revokeToken(id)
      invalidate('tokens')
      toast('Token revoked', { variant: 'success' })
    } catch (err) {
      toastError('Couldn’t revoke the token', err)
    }
  }

  return (
    <Section title="API tokens" description="Let other apps and scripts control Hubbo.">
      <form onSubmit={create} className="mb-5 flex flex-wrap items-end gap-2">
        <div className="min-w-48 flex-1">
          <Field label="Name" htmlFor="tn">
            <Input id="tn" placeholder="Kitchen tablet" value={name} onChange={(e) => setName(e.target.value)} maxLength={48} required />
          </Field>
        </div>
        <Button type="submit" disabled={busy || !name.trim()}>
          {busy ? <Loader2 className="animate-spin" /> : <Plus />} Create token
        </Button>
      </form>
      {tokens.error ? (
        <ErrorState error={tokens.error} retry={tokens.refetch} />
      ) : !tokens.data ? (
        <Skeleton className="h-20" />
      ) : tokens.data.length === 0 ? (
        <p className="text-sm text-muted-foreground">No tokens yet.</p>
      ) : (
        <ul className="divide-y rounded-lg border">
          {tokens.data.map((t) => (
            <li key={t.id} className="flex items-center gap-3 px-4 py-3">
              <KeyRound className="size-4 shrink-0 text-muted-foreground" aria-hidden />
              <div className="min-w-0 flex-1">
                <p className="truncate font-medium">{t.name}</p>
                <p className="text-xs text-muted-foreground">
                  <span className="font-mono">{t.hint}…</span> · Last used {timeAgo(t.last_used_at)}
                </p>
              </div>
              <Button variant="ghost" size="sm" className="text-destructive hover:text-destructive" onClick={() => revoke(t.id)} aria-label={`Revoke ${t.name}`}>
                <Trash2 /> Revoke
              </Button>
            </li>
          ))}
        </ul>
      )}
      <Dialog open={!!created} onClose={() => setCreated(null)} title="Copy your token" description="You won’t see it again." footer={<Button onClick={() => setCreated(null)}>Done</Button>}>
        {created && (
          <div className="grid gap-3">
            <div className="flex items-center gap-2 rounded-lg border bg-muted p-2">
              <code className="min-w-0 flex-1 break-all px-1 font-mono text-sm select-all">{created.token}</code>
              <Button
                size="sm"
                variant="outline"
                onClick={async () => {
                  await navigator.clipboard?.writeText(created.token)
                  setCopied(true)
                }}
              >
                {copied ? <Check /> : <Copy />} {copied ? 'Copied' : 'Copy'}
              </Button>
            </div>
            <p className="text-xs text-muted-foreground">Send it as a Bearer token. Keep it private.</p>
          </div>
        )}
      </Dialog>
    </Section>
  )
}

function RemoteTab() {
  const status = useStatus()
  const last = status.data?.remote.last_event
  return (
    <div className="grid items-start gap-4 lg:grid-cols-[minmax(0,1fr)_14rem]">
      <div className="grid gap-4">
        <Section title="Pair the remote">
          <PairRemote paired={status.data?.remote.paired} />
        </Section>
        <Section title="Channel and address">
          <RemoteEndpoint />
        </Section>
      </div>
      <div className="hidden lg:block">
        <RemoteSvg selected={last && last.age_ms < 5000 ? last.button : null} label="Remote" className="mx-auto max-h-[32rem]" />
      </div>
    </div>
  )
}

/** The word typed to confirm a configuration reset. */
const RESET_WORD = 'reset'

function ResetConfiguration() {
  const [confirm, setConfirm] = useState(false)
  const [typed, setTyped] = useState('')
  const [busy, setBusy] = useState(false)
  const close = () => {
    setConfirm(false)
    setTyped('')
  }
  const reset = async () => {
    setBusy(true)
    try {
      await api.resetConfiguration()
      invalidateAll() // every object has a new revision now
      toast('Configuration reset', { variant: 'success' })
      close()
    } catch (e) {
      toastError('Couldn’t reset the configuration', e)
    } finally {
      setBusy(false)
    }
  }
  return (
    <Section title="Reset configuration" description="Removes devices, activities and integration settings. Keeps your login, tokens and pairings.">
      <Button variant="outline" className="text-destructive hover:text-destructive" onClick={() => setConfirm(true)}>
        <RotateCcw /> Reset configuration…
      </Button>
      <Dialog
        open={confirm}
        onClose={close}
        title="Reset the configuration?"
        description="Your devices, activities and integration settings will be removed."
        footer={
          <>
            <Button variant="ghost" onClick={close}>
              Cancel
            </Button>
            <Button variant="destructive" onClick={reset} disabled={busy || !typedMatches(typed, RESET_WORD)}>
              {busy && <Loader2 className="animate-spin" />} Reset configuration
            </Button>
          </>
        }
      >
        <TypedWordField word={RESET_WORD} id="reset-confirm" value={typed} onChange={setTyped} />
      </Dialog>
    </Section>
  )
}
