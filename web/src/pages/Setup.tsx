import { Check, ChevronRight, Loader2 } from 'lucide-react'
import { useEffect, useState, type FormEvent } from 'react'
import { useLocation } from 'wouter'
import { api, errorMessage } from '@/api/client'
import { HomeAssistantSettings } from '@/components/admin/HomeAssistantForm'
import { NetworkInfo } from '@/components/admin/NetworkInfo'
import { PairRemote } from '@/components/admin/PairRemote'
import { RemoteEndpoint } from '@/components/admin/RemoteEndpoint'
import { ThemeToggle } from '@/components/AppShell'
import { ConfigBanner } from '@/components/ConfigBanner'
import { Logo } from '@/components/Logo'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Field, Input, ShowErrors } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { useIntegrations, useSession, useStatus } from '@/hooks/data'
import { healthLabel } from '@/lib/health'
import { cn } from '@/lib/utils'

const STEPS = [
  { id: 'admin', title: 'Account', blurb: null },
  { id: 'network', title: 'Network', blurb: null },
  { id: 'remote', title: 'Remote', blurb: null },
  { id: 'ha', title: 'Home Assistant', blurb: 'Optional.' },
  { id: 'done', title: 'Done', blurb: 'You’re all set.' },
] as const
type StepId = (typeof STEPS)[number]['id']
const stepIndex = (id: StepId | string | undefined) => STEPS.findIndex((s) => s.id === id)

/**
 * Where to pick up after a reload, from what the hub already has (never from browser storage):
 * the first thing that isn't done yet. Skipped optional steps (pairing, HA) come back until done.
 */
function useResumeStep(needed: boolean): number | null {
  const session = useSession().data
  // Only when the step isn't already known, and once signed in (before the admin exists these are 401s).
  const ask = needed && !!session && !session.setup_required
  const status = useStatus(3000, ask).data
  if (!session) return null
  if (session.setup_required) return stepIndex('admin')
  if (!status) return null
  if (!status.remote.paired) return stepIndex('remote')
  if (status.integrations.find((i) => i.type === 'home_assistant')?.state === 'not_configured') return stepIndex('ha')
  return stepIndex('done')
}

/** The wizard. `start` (from `#/setup/<step>`) opens a given step; otherwise it resumes from hub state. */
export default function Setup({ onDone, start }: { onDone: () => void; start?: string }) {
  const startAt = stepIndex(start)
  const [step, setStep] = useState<number | null>(null)
  const resume = useResumeStep(step === null && startAt <= 0)
  const [, navigate] = useLocation()
  // Resume once, then stay put: pairing at the resumed step mustn't jump the wizard ahead by itself.
  useEffect(() => {
    if (step === null && startAt <= 0 && resume !== null && resume > 0) setStep(resume)
  }, [step, startAt, resume])
  const current = step ?? (startAt > 0 ? startAt : resume)

  const finish = () => {
    navigate('/')
    onDone()
  }
  // Once the admin exists, a reload (or App remounting this as signed in) comes back here instead of
  // the dashboard. Network is in the URL because resuming from hub state never picks it; after it,
  // `#/setup` resumes from the hub.
  const adminCreated = () => {
    navigate('/setup/network')
    setStep(stepIndex('network'))
  }
  const next = () => {
    if (current === stepIndex('network')) navigate('/setup')
    if (current !== null && current < STEPS.length - 1) setStep(current + 1)
    else finish()
  }

  return (
    <div className="min-h-dvh bg-muted/40">
      {/* The config state needs a session, so this appears once the admin exists. */}
      {current !== null && current > 0 && <ConfigBanner admin inSetup />}
      <div className="mx-auto grid max-w-3xl gap-6 px-4 py-8 sm:py-12">
        <header className="flex items-center gap-3">
          <Logo className="size-9" />
          <div>
            <h1 className="text-xl font-semibold">Set up Hubbo</h1>
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
              <h2 id="step-title" className="mt-1 mb-6 text-lg font-semibold">
                {STEPS[current]!.title}
                {STEPS[current]!.blurb && <span className="mt-0.5 block text-sm font-normal text-muted-foreground">{STEPS[current]!.blurb}</span>}
              </h2>
              {
                {
                  admin: <AdminStep onCreated={adminCreated} />,
                  network: <NetworkStep />,
                  remote: <RemoteStep />,
                  ha: <HaStep />,
                  done: <DoneStep />,
                }[STEPS[current]!.id]
              }
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
  const [username, setUsername] = useState('') // no default: the user picks it
  const [password, setPassword] = useState('')
  const [confirm, setConfirm] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const mismatch = confirm.length > 0 && confirm !== password
  const [attempted, setAttempted] = useState(false) // errors show from the first submit on

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setAttempted(true)
    if (mismatch) return
    setBusy(true)
    setError(null)
    try {
      await api.setupAdmin(username, password) // logs in (sets the session cookie)
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
    <ShowErrors.Provider value={attempted}>
    <form onSubmit={submit} className="grid max-w-sm gap-4">
      {error && <Alert variant="error">{error}</Alert>}
      <Field label="Username" htmlFor="su">
        <Input id="su" autoComplete="username" maxLength={32} autoCapitalize="none" autoCorrect="off" spellCheck={false} value={username} onChange={(e) => setUsername(e.target.value)} required autoFocus />
      </Field>
      <Field label="Password" htmlFor="sp" hint="At least 10 characters.">
        <Input id="sp" type="password" autoComplete="new-password" minLength={10} value={password} onChange={(e) => setPassword(e.target.value)} required />
      </Field>
      <Field label="Confirm password" htmlFor="sp2" error={mismatch ? 'Passwords don’t match' : null}>
        <Input id="sp2" type="password" autoComplete="new-password" value={confirm} onChange={(e) => setConfirm(e.target.value)} aria-invalid={mismatch} required />
      </Field>
      <div>
        <Button type="submit" disabled={busy}>
          {busy && <Loader2 className="animate-spin" />} Create account
        </Button>
      </div>
    </form>
    </ShowErrors.Provider>
  )
}

function NetworkStep() {
  return <NetworkInfo bare />
}

function RemoteStep() {
  return (
    <div className="grid gap-6">
      <PairRemote />
      <div className="grid gap-3 border-t pt-5">
        <h3 className="text-sm font-medium">Or enter the channel and address</h3>
        <RemoteEndpoint />
      </div>
    </div>
  )
}

/** Saving connects (and shows the result); Continue or Skip move on. */
function HaStep() {
  const integrations = useIntegrations()
  const ha = integrations.data?.find((i) => i.type === 'home_assistant')
  if (!integrations.data) return <Skeleton className="h-40" />
  if (!ha) return <p className="text-sm text-muted-foreground">Home Assistant isn’t available.</p>
  return <HomeAssistantSettings id={ha.id} showState />
}

/** What's set up, from the hub. */
function DoneStep() {
  const status = useStatus().data
  const ha = status?.integrations.find((i) => i.type === 'home_assistant')
  if (!status) return <Skeleton className="h-24" />
  const rows: [string, string][] = [
    ['Account', 'Created'],
    ['Remote', status.remote.paired ? 'Paired' : 'Not paired'],
    ['Home Assistant', ha ? healthLabel(ha.state) : 'Not available'],
  ]
  return (
    <dl className="grid grid-cols-[auto_1fr] gap-x-6 gap-y-1.5 text-sm" aria-label="Setup summary">
      {rows.map(([k, v]) => (
        <div key={k} className="contents">
          <dt className="text-muted-foreground">{k}</dt>
          <dd>{v}</dd>
        </div>
      ))}
    </dl>
  )
}
