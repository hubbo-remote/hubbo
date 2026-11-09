import { Check, ChevronRight, Loader2, Plus } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useLocation } from 'wouter'
import { api, errorMessage } from '@/api/client'
import { useSession } from '@/hooks/data'
import { AddDeviceDialog } from '@/components/admin/AddDeviceDialog'
import { HomeAssistantSettings } from '@/components/admin/HomeAssistantForm'
import { NetworkInfo } from '@/components/admin/NetworkInfo'
import { PairRemote } from '@/components/admin/PairRemote'
import { ThemeToggle } from '@/components/AppShell'
import { ConfigBanner } from '@/components/ConfigBanner'
import { Logo } from '@/components/Logo'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useDevices, useIntegrations } from '@/hooks/data'
import { providerRef } from '@/lib/health'
import { cn } from '@/lib/utils'

const STEPS = [
  { id: 'admin', title: 'Admin account', blurb: 'One account manages the hub. Pick a strong password; it’s stored hashed on the hub.' },
  { id: 'network', title: 'Network', blurb: 'Hubbo uses wired Ethernet and gets its address from your router.' },
  { id: 'remote', title: 'Pair the remote', blurb: 'Hubbo takes over from your existing Hub by listening on its address.' },
  { id: 'ha', title: 'Home Assistant', blurb: 'Optional. Lets Hubbo control receivers, TVs, lights and switches through Home Assistant.' },
  { id: 'devices', title: 'Devices', blurb: 'Register the things your activities will control. You can add more later.' },
] as const

export default function Setup({ onDone }: { onDone: () => void }) {
  const session = useSession()
  const [step, setStep] = useState<number | null>(null)
  const [, navigate] = useLocation()
  const current = step ?? (session.data ? (session.data.setup_required ? 0 : 1) : null)

  const finish = () => {
    navigate('/')
    onDone()
  }
  const next = () => (current !== null && current < STEPS.length - 1 ? setStep(current + 1) : finish())

  return (
    <div className="min-h-dvh bg-muted/40">
      {/* The config state needs a session, so this appears once the admin exists. */}
      {current !== null && current > 0 && <ConfigBanner admin link={false} />}
      <div className="mx-auto grid max-w-3xl gap-6 px-4 py-8 sm:py-12">
        <header className="flex items-center gap-3">
          <Logo className="size-9" />
          <div>
            <h1 className="text-xl font-semibold">Set up Hubbo</h1>
            <p className="text-sm text-muted-foreground">About five minutes. Everything here can be changed later.</p>
          </div>
          <ThemeToggle className="ml-auto" />
        </header>

        <ol className="flex gap-1.5" aria-label="Setup progress">
          {STEPS.map((s, i) => (
            <li key={s.id} className="flex-1" aria-current={i === current ? 'step' : undefined}>
              <div className={cn('h-1.5 rounded-full bg-border transition-colors', current !== null && i <= current && 'bg-primary')} />
              <p className={cn('mt-1.5 hidden text-xs sm:block', i === current ? 'font-medium text-foreground' : 'text-muted-foreground')}>
                {i < (current ?? 0) && <Check className="mr-1 inline size-3 text-success" />}
                {s.title}
              </p>
            </li>
          ))}
        </ol>

        <section className="rounded-2xl border bg-card p-5 shadow-sm sm:p-7" aria-labelledby="step-title">
          {current === null ? (
            <Skeleton className="h-60" />
          ) : (
            <>
              <p className="text-xs font-medium text-muted-foreground">
                Step {current + 1} of {STEPS.length}
              </p>
              <h2 id="step-title" className="mt-1 text-lg font-semibold">
                {STEPS[current]!.title}
              </h2>
              <p className="mb-6 text-sm text-muted-foreground">{STEPS[current]!.blurb}</p>
              {current === 0 && <AdminStep onCreated={next} />}
              {current === 1 && <NetworkStep />}
              {current === 2 && <PairRemote />}
              {current === 3 && <HaStep onSaved={next} />}
              {current === 4 && <DevicesStep />}
            </>
          )}
        </section>

        {current !== null && current > 0 && (
          <div className="flex items-center justify-between gap-2">
            <Button variant="ghost" onClick={() => setStep(current - 1)} disabled={current === 1}>
              Back
            </Button>
            <div className="flex gap-2">
              {current < STEPS.length - 1 && (
                <Button variant="ghost" onClick={next}>
                  Skip
                </Button>
              )}
              <Button onClick={next}>
                {current === STEPS.length - 1 ? 'Finish' : 'Continue'}
                <ChevronRight />
              </Button>
            </div>
          </div>
        )}
      </div>
    </div>
  )
}

function AdminStep({ onCreated }: { onCreated: () => void }) {
  const [username, setUsername] = useState('admin')
  const [password, setPassword] = useState('')
  const [confirm, setConfirm] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const mismatch = confirm.length > 0 && confirm !== password

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (mismatch) return
    setBusy(true)
    setError(null)
    try {
      await api.setupAdmin(username, password) // logs in; the response carries the CSRF token
      setPassword('')
      setConfirm('')
      onCreated()
    } catch (err) {
      setError(errorMessage(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <form onSubmit={submit} className="grid max-w-sm gap-4">
      {error && <Alert variant="error">{error}</Alert>}
      <Field label="Username" htmlFor="su">
        <Input id="su" autoComplete="username" value={username} onChange={(e) => setUsername(e.target.value)} required />
      </Field>
      <Field label="Password" htmlFor="sp" hint="At least 10 characters.">
        <Input id="sp" type="password" autoComplete="new-password" minLength={10} value={password} onChange={(e) => setPassword(e.target.value)} required autoFocus />
      </Field>
      <Field label="Confirm password" htmlFor="sp2" error={mismatch ? 'Passwords don’t match' : null}>
        <Input id="sp2" type="password" autoComplete="new-password" value={confirm} onChange={(e) => setConfirm(e.target.value)} aria-invalid={mismatch} required />
      </Field>
      <div>
        <Button type="submit" disabled={busy || mismatch}>
          {busy && <Loader2 className="animate-spin" />} Create account
        </Button>
      </div>
    </form>
  )
}

function NetworkStep() {
  return <NetworkInfo bare />
}

function HaStep({ onSaved }: { onSaved: () => void }) {
  const integrations = useIntegrations()
  const ha = integrations.data?.find((i) => i.type === 'home_assistant')
  if (!integrations.data) return <Skeleton className="h-40" />
  if (!ha) return <p className="text-sm text-muted-foreground">This hub has no Home Assistant integration.</p>
  return <HomeAssistantSettings id={ha.id} onSaved={onSaved} />
}

function DevicesStep() {
  const devices = useDevices()
  const [adding, setAdding] = useState(false)
  return (
    <div className="grid gap-4">
      {devices.data && devices.data.length > 0 ? (
        <ul className="divide-y rounded-lg border">
          {devices.data.map((d) => (
            <li key={d.id} className="flex items-center justify-between gap-3 px-4 py-2.5 text-sm">
              <span className="font-medium">{d.name}</span>
              <span className="truncate font-mono text-xs text-muted-foreground">{providerRef(d.provider)}</span>
            </li>
          ))}
        </ul>
      ) : (
        <p className="text-sm text-muted-foreground">No devices yet.</p>
      )}
      <div>
        <Button variant="outline" onClick={() => setAdding(true)}>
          <Plus /> Add device
        </Button>
      </div>
      <AddDeviceDialog open={adding} onClose={() => setAdding(false)} />
    </div>
  )
}
