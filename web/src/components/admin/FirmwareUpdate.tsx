import { ArrowUpCircle, Download, ExternalLink, FileUp, Loader2, RefreshCw, Upload, X } from 'lucide-react'
import { useEffect, useRef, useState, type ReactNode, type RefObject } from 'react'
import { api, ApiError, errorMessage, toApiError } from '@/api/client'
import { getQueryData, invalidate, setQueryData, useQuery } from '@/api/hooks'
import type { FirmwareRelease, FirmwareStatus } from '@/api/types'
import { ErrorState, Section } from '@/components/common'
import { useHub } from '@/hooks/data'
import { Alert } from '@/components/ui/alert'
import { Button, buttonVariants } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Skeleton } from '@/components/ui/misc'
import { Switch } from '@/components/ui/switch'
import { toastError } from '@/components/ui/toast'
import { readMarker, releaseNotesUrl, type ImageMarker } from '@/lib/firmware'
import { NOT_BACK, RESTART_SLOW_MS } from '@/lib/restart'
import { agoMs, cn, sleep } from '@/lib/utils'

const POLL_MS = 1000

/**
 * The update in flight: the version it started from and the one the file carries. Module memory
 * (never browser storage), so it outlives the sign-in the reboot asks for: the section comes back
 * after it and checks what's running now.
 */
let pending: { from: string; expected: string | null } | null = null

type Phase =
  | { step: 'idle' }
  | { step: 'uploading'; fraction: number }
  | { step: 'downloading'; progress: number }
  | { step: 'writing'; progress: number }
  | { step: 'restarting'; slow: boolean }
  /** After the restart: `running` is the version the hub reported then, fresh. */
  | { step: 'done'; from: string; expected: string | null; running: string }
  | { step: 'refused'; error: ApiError }

type InFlight = Extract<Phase, { step: 'uploading' | 'downloading' | 'writing' | 'restarting' }>
const inFlight = (p: Phase): p is InFlight => p.step === 'uploading' || p.step === 'downloading' || p.step === 'writing' || p.step === 'restarting'
/** Where the hub's update is, as an in-flight phase; null when none runs (idle, or failed). */
function phaseOf(s: FirmwareStatus): InFlight | null {
  const { state, progress } = s.update
  if (state === 'downloading' || state === 'writing') return { step: state, progress }
  return state === 'rebooting' ? { step: 'restarting', slow: false } : null
}

/** The hub isn't answering: it's restarting (a proxy in front of it answers 502/503/504 meanwhile). */
const notAnswering = (e: ApiError) => e.status === 0 || e.status === 502 || e.status === 504 || (e.status === 503 && e.code !== 'unavailable')

export function FirmwareUpdate() {
  // Follow a check (ours, or one the hub runs by itself) until it's done: the last answer says one runs.
  const fw = useQuery('firmware', () => api.firmware(), { pollMs: getQueryData<FirmwareStatus>('firmware')?.checking ? POLL_MS : undefined })
  const [file, setFile] = useState<{ file: File; marker: ImageMarker | null } | null>(null)
  // Back after the sign-in the reboot asked for: follow on until a fresh answer says what's running.
  const [phase, setPhase] = useState<Phase>(() => (pending ? { step: 'restarting', slow: false } : { step: 'idle' }))
  /** The version being installed, for the update block (null: an unknown file, or a watch picked up mid-way). */
  const [target, setTarget] = useState<string | null>(() => pending?.expected ?? null)
  // Bumped by each upload or watch, and by unmounting: only the latest one updates the section
  // (StrictMode's trial mount starts a watch the real mount supersedes).
  const run = useRef(0)
  useEffect(() => {
    if (pending) void watch(++run.current)
    // Unmounting stops the running watch (the ref holds a counter, not a node).
    // eslint-disable-next-line react-hooks/exhaustive-deps
    return () => void run.current++
  }, [])

  /** Polls GET /admin/firmware through the download, the write and the reboot, until the hub answers idle (or 401: sign in again). */
  const watch = async (id: number) => {
    const started = Date.now()
    for (;;) {
      await sleep(POLL_MS)
      if (run.current !== id) return
      const answer = await api.firmware().then((s) => ({ s }), (e: unknown) => ({ err: toApiError(e) }))
      if (run.current !== id) return
      if ('s' in answer) {
        const { s } = answer
        setQueryData('firmware', s)
        const next = phaseOf(s)
        if (next) setPhase(next)
        else {
          // idle: the hub is back (on the board, after a reboot). failed shows from the status itself.
          setPhase(s.update.state === 'idle' && pending ? { step: 'done', ...pending, running: s.version } : { step: 'idle' })
          pending = null
          invalidate('status')
          return
        }
      } else {
        const { err } = answer
        // 401: the hub restarted and every session is gone; the app has switched to the sign-in.
        if (err.status === 401) return
        if (!notAnswering(err)) return setPhase({ step: 'refused', error: err })
        setPhase({ step: 'restarting', slow: Date.now() - started > RESTART_SLOW_MS })
      }
    }
  }

  // A reload (or another tab) mid-update: keep following it.
  const state = fw.data?.update.state
  useEffect(() => {
    const next = fw.data && phaseOf(fw.data)
    if (!next || phase.step !== 'idle') return
    setPhase(next)
    void watch(++run.current)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [state])

  const fileInput = useRef<HTMLInputElement>(null)
  const clearFile = () => {
    if (fileInput.current) fileInput.current.value = '' // picking the same file again still fires a change
    void choose(undefined)
  }
  const choose = async (f: File | undefined) => {
    setFile(f ? { file: f, marker: readMarker(new Uint8Array(await f.arrayBuffer())) } : null)
    if (phase.step === 'refused' || phase.step === 'done') setPhase({ step: 'idle' })
  }

  /** Starts an install (`send`, which answers the hub's status), then follows it; only the latest one updates the section. */
  const start = async (expected: string | null, first: InFlight, send: (id: number) => Promise<FirmwareStatus>) => {
    const id = ++run.current
    setTarget(expected)
    setPhase(first)
    try {
      const s = await send(id)
      pending = { from: s.version, expected }
      setQueryData('firmware', s)
      if (run.current !== id) return
      setPhase(phaseOf(s) ?? first)
      await watch(id)
    } catch (e) {
      if (run.current === id) setPhase({ step: 'refused', error: toApiError(e) })
      invalidate('firmware') // e.g. not_available: a newer check replaced it
    }
  }
  const installRelease = (release: FirmwareRelease) => start(release.version, { step: 'downloading', progress: 0 }, () => api.installFirmware(release.version))
  const install = (f: NonNullable<typeof file>) =>
    start(f.marker?.version ?? null, { step: 'uploading', fraction: 0 }, (id) => api.uploadFirmware(f.file, (fraction) => run.current === id && setPhase({ step: 'uploading', fraction })))

  const current = inFlight(phase) ? phase : null
  const busy = !!current
  if (!fw.data) return <Section title="Firmware">{fw.error ? <ErrorState error={fw.error} retry={fw.refetch} /> : <Skeleton className="h-10" />}</Section>
  const s = fw.data
  const updating = busy || !!phaseOf(s)
  return (
    <Section title="Firmware">
      <div className="grid gap-4">
        <About fw={s} />
        {current ? (
          <UpdateBlock title={target ? `Updating to Hubbo ${target}` : 'Updating Hubbo'}>
            <Progress phase={current} />
          </UpdateBlock>
        ) : (
          s.available && <Available release={s.available} disabled={updating} onInstall={(r) => void installRelease(r)} />
        )}
        <Outcome fw={s} phase={phase} />
        <Checks fw={s} updating={updating} actions={<ChooseFile input={fileInput} disabled={busy} onChoose={(f) => void choose(f)} />}>
          {file && (
            <div className="flex flex-wrap items-center gap-x-3 gap-y-2 border-t pt-3">
              <div className="grid min-w-0 gap-0.5">
                <span className="inline-flex min-w-0 items-center gap-1 text-sm">
                  <span className="max-w-56 truncate" title={file.file.name}>
                    {file.file.name}
                  </span>
                  <Button variant="ghost" size="icon" className="size-7" aria-label="Clear file" onClick={clearFile} disabled={busy}>
                    <X />
                  </Button>
                </span>
                <span className="text-xs text-muted-foreground">{file.marker ? `Hubbo ${file.marker.version} for ${file.marker.board}` : 'This doesn’t look like a Hubbo update.'}</span>
              </div>
              <Button size="sm" className="ml-auto" onClick={() => void install(file)} disabled={busy}>
                <Upload /> Update
              </Button>
            </div>
          )}
        </Checks>
      </div>
    </Section>
  )
}

const CHECK_ERROR: Record<NonNullable<FirmwareStatus['check_error']>, string> = {
  unreachable: 'Hubbo couldn’t reach the internet to check for updates.',
  bad_signature: 'Hubbo found an update but couldn’t confirm it’s genuine, so it ignored it.',
  bad_manifest: 'Hubbo found an update but couldn’t read it, so it ignored it.',
}

/** Board ids as their makers name them; any other shows as reported. */
const BOARD_NAMES: Record<string, string> = { waveshare_esp32_s3_eth: 'Waveshare ESP32-S3-ETH' }

/** What's running, like a device's About panel. */
function About({ fw }: { fw: FirmwareStatus }) {
  return (
    <div className="grid gap-1">
      <p className="text-xl font-semibold tracking-tight">Hubbo {fw.version}</p>
      <dl className="flex gap-2 text-sm">
        <dt className="text-muted-foreground">Board</dt>
        <dd>{BOARD_NAMES[fw.board] ?? fw.board}</dd>
      </dl>
    </div>
  )
}

/** Checking for updates: the daily switch and Check now together, with where the checks stand. */
function Checks({ fw, updating, actions, children }: { fw: FirmwareStatus; updating: boolean; actions: ReactNode; children?: ReactNode }) {
  const [starting, setStarting] = useState(false)
  const checking = fw.checking || starting
  const check = async () => {
    setStarting(true)
    try {
      setQueryData('firmware', await api.checkFirmware()) // checking: the card polls until it's done
    } catch (e) {
      // busy: a check is already running; a fresh status says so, and the card follows it.
      if (e instanceof ApiError && e.code === 'busy') invalidate('firmware')
      else toastError('Couldn’t check for updates', e)
    } finally {
      setStarting(false)
    }
  }
  const checked = fw.checked_ago_ms === null ? null : agoMs(fw.checked_ago_ms)
  // Nothing the update block already shows (an update, or one installing). While a check runs the line stays
  // as it was (Check now's spinner says it; screen readers hear it below).
  const state = fw.check_error
    ? `${CHECK_ERROR[fw.check_error]}${checked ? ` Last checked ${checked}.` : ''}`
    : fw.available || updating
      ? null
      : checked && `Hubbo is up to date. Last checked ${checked}.`
  return (
    <div className="grid gap-2 rounded-lg border p-3">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <CheckSetting />
        <div className="flex flex-wrap items-center gap-2">
          <Button variant="outline" size="sm" onClick={() => void check()} disabled={checking || updating}>
            <RefreshCw className={cn(checking && 'animate-spin')} /> Check now
          </Button>
          {actions}
        </div>
      </div>
      {state && (
        <p className={cn('text-xs', fw.check_error && !checking ? 'text-destructive' : 'text-muted-foreground')} aria-live="polite">
          {state}
        </p>
      )}
      <span className="sr-only" aria-live="polite">
        {checking ? 'Checking for updates…' : ''}
      </span>
      {children}
    </div>
  )
}

/** The accent block an update lives in: what's available, then its progress, in the same place. */
function UpdateBlock({ title, children, action }: { title: string; children?: ReactNode; action?: ReactNode }) {
  return (
    <div className="grid gap-3 rounded-lg border border-primary/30 bg-primary/5 p-4">
      <div className="flex flex-wrap items-center justify-between gap-3">
        <p className="inline-flex items-center gap-2 font-medium">
          <ArrowUpCircle className="size-5 text-primary" /> {title}
        </p>
        {action}
      </div>
      {children}
    </div>
  )
}

function Available({ release, disabled, onInstall }: { release: FirmwareRelease; disabled: boolean; onInstall: (r: FirmwareRelease) => void }) {
  const [confirm, setConfirm] = useState(false)
  return (
    <UpdateBlock
      title={`Hubbo ${release.version} is available`}
      action={
        <Button onClick={() => setConfirm(true)} disabled={disabled}>
          <Download /> Install
        </Button>
      }
    >
      <a href={releaseNotesUrl(release.version)} target="_blank" rel="noreferrer" className="inline-flex w-fit items-center gap-1 text-sm text-primary hover:underline">
        What’s new <ExternalLink className="size-3.5" />
      </a>
      <Dialog
        open={confirm}
        onClose={() => setConfirm(false)}
        title={`Update to Hubbo ${release.version}?`}
        description="Your remote won’t work for about a minute while Hubbo updates and restarts. You’ll need to sign in again afterwards."
        footer={
          <>
            <Button variant="outline" onClick={() => setConfirm(false)}>
              Cancel
            </Button>
            <Button
              onClick={() => {
                setConfirm(false)
                onInstall(release)
              }}
            >
              Update
            </Button>
          </>
        }
      />
    </UpdateBlock>
  )
}

function Progress({ phase }: { phase: InFlight }) {
  const [label, percent, text] =
    phase.step === 'uploading'
      ? ['Upload progress', Math.round(phase.fraction * 100), 'Uploading the file…']
      : phase.step === 'downloading'
        ? ['Download progress', phase.progress, 'Downloading the update…']
        : phase.step === 'writing'
          ? ['Write progress', phase.progress, 'Installing the update…']
          : [null, 100, phase.slow ? NOT_BACK : 'Restarting Hubbo…']
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

/** Why an install stopped (update.error). */
const FAILED: Record<string, string> = {
  download_failed: 'The download didn’t finish.',
  image_mismatch: 'The download came through damaged.',
  invalid_image: 'It isn’t an update this Hubbo can use.',
}

/**
 * How the last update ended, as one alert, when there's something to say: refused, failed, installed or not
 * after the restart, or rolled back. A reset during the write, or a new image that crashes early, boots the old
 * one again (the failed state, in RAM, is gone then; a rollback says so).
 */
function Outcome({ fw, phase }: { fw: FirmwareStatus; phase: Phase }) {
  if (phase.step === 'refused') {
    const { error } = phase
    const title =
      error.code === 'update_in_progress' ? 'Another update is already running'
      : error.code === 'not_available' ? 'That update isn’t available any more'
      : error.status === 503 ? 'Hubbo can’t update right now'
      : error.status === 0 ? 'The file didn’t upload'
      : 'Hubbo didn’t install this file'
    return (
      <Alert variant="error" title={title}>
        <p>{errorMessage(error)} Nothing on Hubbo has changed.</p>
      </Alert>
    )
  }
  if (fw.update.state === 'failed')
    return (
      <Alert variant="error" title="The update didn’t install">
        <p>
          {FAILED[fw.update.error ?? ''] ?? ''} Hubbo is still running {fw.version}, so nothing has changed.
        </p>
      </Alert>
    )
  if (fw.rolled_back_from)
    return (
      <Alert variant="warning" title="The update didn’t start">
        <p>
          The update to {fw.rolled_back_from} didn’t start, so Hubbo went back to {fw.version}.
        </p>
      </Alert>
    )
  if (phase.step !== 'done') return null
  const { from, expected, running } = phase
  const took = expected ? running === expected : running !== from
  return took ? (
    <Alert variant="success" title={`Hubbo is now running ${running}.`} />
  ) : (
    <Alert variant="error" title="The update didn’t install">
      <p>
        Hubbo restarted but is still running {running}
        {expected && ` instead of ${expected}`}.
      </p>
    </Alert>
  )
}

/** "Check for updates": the hub checks once a day; off, only Check now does. */
function CheckSetting() {
  const hub = useHub()
  const [saving, setSaving] = useState(false)
  if (!hub.data) return null
  const toggle = async (on: boolean) => {
    setSaving(true)
    try {
      setQueryData('hub', await api.setCheckUpdates(on))
    } catch (e) {
      toastError('Couldn’t save the setting', e)
    } finally {
      setSaving(false)
    }
  }
  return (
    <label className="flex items-center gap-3 text-sm">
      <Switch checked={hub.data.check_updates} onCheckedChange={(on) => void toggle(on)} disabled={saving} aria-label="Check for updates daily" />
      Check for updates daily
    </label>
  )
}

/** "Choose file…": an outline button over the native input (hidden, labelled "Firmware file"). */
function ChooseFile({ input, disabled, onChoose }: { input: RefObject<HTMLInputElement | null>; disabled: boolean; onChoose: (f: File | undefined) => void }) {
  return (
    <>
      <input ref={input} id="firmware-file" type="file" accept=".bin,application/octet-stream" aria-label="Firmware file" className="peer sr-only" disabled={disabled} onChange={(e) => onChoose(e.target.files?.[0])} />
      <label htmlFor="firmware-file" className={cn(buttonVariants({ variant: 'outline', size: 'sm' }), 'cursor-pointer peer-focus-visible:ring-[3px] peer-focus-visible:ring-ring/50', disabled && 'pointer-events-none opacity-50')}>
        <FileUp /> Choose file…
      </label>
    </>
  )
}
