import { Loader2, Upload } from 'lucide-react'
import { useEffect, useRef, useState } from 'react'
import { api, ApiError, errorMessage, toApiError } from '@/api/client'
import { invalidate, setQueryData, useQuery } from '@/api/hooks'
import { ErrorState, Facts, Section } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { readMarker, type ImageMarker } from '@/lib/firmware'
import { sleep } from '@/lib/utils'

const POLL_MS = 1000
/** Writing takes well under a minute and a reboot seconds; after this long without the hub, say so. */
const GIVE_UP_MS = 3 * 60_000

/**
 * The update in flight: the version it started from and the one the file carries. Module memory
 * (never browser storage), so it outlives the sign-in the reboot asks for: the section comes back
 * after it and checks what's running now.
 */
let pending: { from: string; expected: string | null } | null = null

type Phase =
  | { step: 'idle' }
  | { step: 'uploading'; fraction: number }
  | { step: 'writing'; progress: number }
  | { step: 'restarting'; slow: boolean }
  /** After the restart: `running` is the version the hub reported then, fresh. */
  | { step: 'done'; from: string; expected: string | null; running: string }
  | { step: 'refused'; error: ApiError }

type InFlight = Extract<Phase, { step: 'uploading' | 'writing' | 'restarting' }>
const inFlight = (p: Phase): p is InFlight => p.step === 'uploading' || p.step === 'writing' || p.step === 'restarting'

/** The hub isn't answering: it's restarting (a proxy in front of it answers 502/503/504 meanwhile). */
const notAnswering = (e: ApiError) => e.status === 0 || e.status === 502 || e.status === 504 || (e.status === 503 && e.code !== 'unavailable')

export function FirmwareUpdate() {
  const fw = useQuery('firmware', () => api.firmware())
  const [file, setFile] = useState<{ file: File; marker: ImageMarker | null } | null>(null)
  // Back after the sign-in the reboot asked for: follow on until a fresh answer says what's running.
  const [phase, setPhase] = useState<Phase>(() => (pending ? { step: 'restarting', slow: false } : { step: 'idle' }))
  // Bumped by each upload or watch, and by unmounting: only the latest one updates the section
  // (StrictMode's trial mount starts a watch the real mount supersedes).
  const run = useRef(0)
  useEffect(() => {
    if (pending) void watch(++run.current, phase)
    // Unmounting stops the running watch (the ref holds a counter, not a node).
    // eslint-disable-next-line react-hooks/exhaustive-deps
    return () => void run.current++
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  /** Polls GET /admin/firmware through the write and the reboot, until the hub answers idle (or 401: sign in again). */
  const watch = async (id: number, from: Phase = { step: 'writing', progress: 0 }) => {
    setPhase(from)
    const started = Date.now()
    for (;;) {
      await sleep(POLL_MS)
      if (run.current !== id) return
      const answer = await api.firmware().then((s) => ({ s }), (e: unknown) => ({ err: toApiError(e) }))
      if (run.current !== id) return
      if ('s' in answer) {
        const { s } = answer
        setQueryData('firmware', s)
        const { state, progress } = s.update
        if (state === 'writing') setPhase({ step: 'writing', progress })
        else if (state === 'rebooting') setPhase({ step: 'restarting', slow: false })
        else {
          // idle: the hub is back (on the board, after a reboot). failed shows from the status itself.
          setPhase(state === 'idle' && pending ? { step: 'done', ...pending, running: s.version } : { step: 'idle' })
          pending = null
          invalidate('status')
          return
        }
      } else {
        const { err } = answer
        // 401: the hub restarted and every session is gone; the app has switched to the sign-in.
        if (err.status === 401) return
        if (!notAnswering(err)) return setPhase({ step: 'refused', error: err })
        setPhase({ step: 'restarting', slow: Date.now() - started > GIVE_UP_MS })
      }
    }
  }

  // A reload (or another tab) mid-update: keep following it.
  const state = fw.data?.update.state
  useEffect(() => {
    if ((state === 'writing' || state === 'rebooting') && phase.step === 'idle') void watch(++run.current)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state])

  const choose = async (f: File | undefined) => {
    setFile(f ? { file: f, marker: readMarker(new Uint8Array(await f.arrayBuffer())) } : null)
    if (phase.step === 'refused' || phase.step === 'done') setPhase({ step: 'idle' })
  }

  const install = async () => {
    if (!file) return
    const id = ++run.current
    setPhase({ step: 'uploading', fraction: 0 })
    try {
      const s = await api.uploadFirmware(file.file, (fraction) => run.current === id && setPhase({ step: 'uploading', fraction }))
      pending = { from: s.version, expected: file.marker?.version ?? null }
      setQueryData('firmware', s)
      if (run.current === id) await watch(id)
    } catch (e) {
      if (run.current === id) setPhase({ step: 'refused', error: toApiError(e) })
    }
  }

  const busy = inFlight(phase)
  return (
    <Section title="Firmware">
      <div className="grid gap-5">
        {fw.data ? (
          <Facts
            rows={[
              ['Version', fw.data.version],
              ['Board', fw.data.board],
            ]}
          />
        ) : fw.error ? (
          <ErrorState error={fw.error} retry={fw.refetch} />
        ) : (
          <Skeleton className="h-10" />
        )}
        <div className="grid gap-2">
          <div className="flex flex-wrap items-center gap-3">
            <label className="inline-flex">
              <span className="sr-only">Firmware file</span>
              <Input type="file" accept=".bin,application/octet-stream" disabled={busy} onChange={(e) => void choose(e.target.files?.[0])} className="max-w-xs" />
            </label>
            <Button onClick={install} disabled={!file || busy}>
              {busy ? <Loader2 className="animate-spin" /> : <Upload />} Install update
            </Button>
          </div>
          {file && (
            <p className="text-xs text-muted-foreground">
              {file.marker ? `Hubbo ${file.marker.version} for ${file.marker.board}` : 'This doesn’t look like a Hubbo update.'}
            </p>
          )}
        </div>
        <p className="text-xs text-muted-foreground">The remote may lag while it installs. You’ll sign in again afterwards.</p>
        <Progress phase={phase} />
        {phase.step === 'refused' && <Refused error={phase.error} />}
        {phase.step === 'done' && <Restarted {...phase} />}
        {fw.data?.update.state === 'failed' && phase.step !== 'refused' && (
          <Alert variant="error" title="The update failed">
            <p>Still running {fw.data.version}. Try again.</p>
          </Alert>
        )}
      </div>
    </Section>
  )
}

function Progress({ phase }: { phase: Phase }) {
  if (!inFlight(phase)) return null
  const [label, percent, text] =
    phase.step === 'uploading'
      ? ['Upload progress', Math.round(phase.fraction * 100), 'Uploading…']
      : phase.step === 'writing'
        ? ['Write progress', phase.progress, 'Installing…']
        : [null, 100, phase.slow ? 'Hubbo hasn’t come back. Check that it’s on and connected.' : 'Restarting…']
  return (
    <div className="grid gap-2">
      <span className="inline-flex items-center gap-2 text-sm" aria-live="polite">
        <Loader2 className="size-4 animate-spin text-primary" /> {text} {label && `${percent}%`}
      </span>
      {label && (
        <div role="progressbar" aria-label={label} aria-valuemin={0} aria-valuemax={100} aria-valuenow={percent} className="h-1.5 w-full overflow-hidden rounded-full bg-muted">
          <div className="h-full bg-primary transition-[width] duration-300" style={{ width: `${percent}%` }} />
        </div>
      )}
    </div>
  )
}

/**
 * The hub restarted: installed only if it runs the file's version. A reset during the write, or a
 * new image that crashes early, boots the old one again (and the failed state, in RAM, is gone).
 */
function Restarted({ from, expected, running }: { from: string; expected: string | null; running: string }) {
  const took = expected ? running === expected : running !== from
  return took ? (
    <Alert variant="success" title="Update installed">
      <p>
        Now running {running} (was {from}).
      </p>
    </Alert>
  ) : (
    <Alert variant="error" title="The update didn’t take">
      <p>
        Still running {running}
        {expected && `, not ${expected}`}. Try again.
      </p>
    </Alert>
  )
}

/** Every refusal comes before anything is written: say so, with the hub's reason. */
function Refused({ error }: { error: ApiError }) {
  const title =
    error.code === 'update_in_progress' ? 'An update is already running'
    : error.status === 503 ? 'Hubbo can’t update right now'
    : error.status === 0 ? 'The upload failed'
    : 'This file wasn’t installed'
  return (
    <Alert variant="error" title={title}>
      <p>{errorMessage(error)}</p>
      <p>Nothing changed.</p>
    </Alert>
  )
}
