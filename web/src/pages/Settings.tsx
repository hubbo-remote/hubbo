import { Check, Copy, Download, KeyRound, Loader2, Plus, RefreshCw, RotateCcw, Trash2, Upload } from 'lucide-react'
import { useState, type FormEvent } from 'react'
import { useLocation } from 'wouter'
import { api, errorMessage, toApiError, type BackupKind } from '@/api/client'
import { invalidate, invalidateAll, useQuery } from '@/api/hooks'
import type { RestoreResult, TokenCreated } from '@/api/types'
import { PairRemote } from '@/components/admin/PairRemote'
import { NetworkInfo } from '@/components/admin/NetworkInfo'
import { ErrorState, PageHeader, Section } from '@/components/common'
import { ThemeToggle } from '@/components/AppShell'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { Tabs } from '@/components/ui/tabs'
import { toast, toastError } from '@/components/ui/toast'
import { useConfigLock, useStatus } from '@/hooks/data'
import { agoMs, timeAgo } from '@/lib/utils'

const TABS = [
  { id: 'account', label: 'Account' },
  { id: 'tokens', label: 'API tokens' },
  { id: 'remote', label: 'Remote' },
  { id: 'network', label: 'Network' },
  { id: 'backup', label: 'Backup & reset' },
  { id: 'firmware', label: 'Firmware' },
  { id: 'diagnostics', label: 'Diagnostics' },
]

const REQUIRES: Record<RestoreResult['requires'][number], string> = {
  home_assistant_token: 'enter the Home Assistant token again',
  ble_pairing: 'pair Bluetooth again',
  api_tokens: 'create new API tokens',
}

export default function Settings({ tab }: { tab: string }) {
  const [, navigate] = useLocation()
  const value = TABS.some((t) => t.id === tab) ? tab : 'account'
  return (
    <>
      <PageHeader title="Settings" />
      <Tabs tabs={TABS} value={value} onChange={(t) => navigate(`/settings/${t}`, { replace: true })}>
        {value === 'account' && <Account />}
        {value === 'tokens' && <Tokens />}
        {value === 'remote' && <RemoteTab />}
        {value === 'network' && <Network />}
        {value === 'backup' && <Backup />}
        {value === 'firmware' && <Firmware />}
        {value === 'diagnostics' && <Diagnostics />}
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

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (mismatch) return
    setBusy(true)
    setError(null)
    try {
      await api.changePassword(cur, next)
      setCur('')
      setNext('')
      setConfirm('')
      toast('Password changed', { variant: 'success', description: 'Other sessions have been signed out.' })
    } catch (err) {
      setError(errorMessage(err))
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="grid gap-4">
      <Section title="Admin password" description="The single admin account manages devices, integrations, pairing, tokens, backups and firmware.">
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
            <Button type="submit" disabled={busy || mismatch}>
              {busy && <Loader2 className="animate-spin" />} Change password
            </Button>
          </div>
        </form>
      </Section>
      <Section title="Setup wizard" description="Walk through network, remote pairing, Home Assistant and devices again.">
        <a href="#/setup" className="text-sm font-medium text-primary underline-offset-4 hover:underline">
          Run setup again
        </a>
      </Section>
      <Section title="Appearance">
        <div className="flex items-center justify-between gap-4">
          <p className="text-sm text-muted-foreground">Theme for this browser.</p>
          <ThemeToggle />
        </div>
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
      toastError('Could not create token', err)
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
      toastError('Could not revoke', err)
    }
  }

  return (
    <Section title="API tokens" description="For scripts, Home Assistant automations and LLM agents. Tokens can list devices, edit activities, test commands and switch activities. They can’t manage devices, integrations, pairing, tokens, backups or firmware.">
      <form onSubmit={create} className="mb-5 flex flex-wrap items-end gap-2">
        <div className="min-w-48 flex-1">
          <Field label="New token name" htmlFor="tn">
            <Input id="tn" placeholder="e.g. Kitchen tablet" value={name} onChange={(e) => setName(e.target.value)} maxLength={48} required />
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
        <p className="text-sm text-muted-foreground">No tokens.</p>
      ) : (
        <ul className="divide-y rounded-lg border">
          {tokens.data.map((t) => (
            <li key={t.id} className="flex items-center gap-3 px-4 py-3">
              <KeyRound className="size-4 shrink-0 text-muted-foreground" aria-hidden />
              <div className="min-w-0 flex-1">
                <p className="truncate font-medium">{t.name}</p>
                <p className="text-xs text-muted-foreground">
                  <span className="font-mono">{t.hint}…</span> · created {t.created_at ? timeAgo(t.created_at) : 'unknown'} · last used {timeAgo(t.last_used_at)}
                </p>
              </div>
              <Button variant="ghost" size="sm" className="text-destructive hover:text-destructive" onClick={() => revoke(t.id)} aria-label={`Revoke ${t.name}`}>
                <Trash2 /> Revoke
              </Button>
            </li>
          ))}
        </ul>
      )}
      <Dialog
        open={!!created}
        onClose={() => setCreated(null)}
        title="Copy your new token"
        description="This is the only time it’s shown. The hub keeps only a hash."
        footer={<Button onClick={() => setCreated(null)}>I’ve saved it</Button>}
      >
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
            <p className="text-xs text-muted-foreground">
              Use it as <code className="font-mono">Authorization: Bearer …</code>. It can still control registered devices, so treat it like a key.
            </p>
          </div>
        )}
      </Dialog>
    </Section>
  )
}

function RemoteTab() {
  const status = useStatus()
  return (
    <Section title="Remote pairing" description="Hubbo listens on the radio address of a Hub the remote is already paired with.">
      <PairRemote paired={status.data?.remote.paired} channel={status.data?.remote.channel} />
    </Section>
  )
}

function Network() {
  return <NetworkInfo />
}

/** Shown before and after a `blocked-raw` download. */
const BLOCKED_RAW_NOTE = 'This is the hub’s saved configuration exactly as stored, exported while the hub is blocked. It may only be restorable on firmware that can read it.'

/** Why a download failed, in the admin's terms (409 config_blocked, 503 storage_unavailable). */
function DownloadError({ error }: { error: unknown }) {
  const e = toApiError(error)
  if (e.code === 'config_blocked')
    return (
      <Alert variant="error" title="Nothing intact to download">
        <p>The saved configuration is damaged and the hub has no intact copy to export, so there’s no backup to download. Restore a backup or reset to factory defaults to continue.</p>
        <p className="mt-1 text-muted-foreground">The hub said: {e.message}</p>
      </Alert>
    )
  if (e.code === 'storage_unavailable')
    return (
      <Alert variant="error" title="The hub can’t read its storage">
        <p>No backup was made: the saved configuration can’t be read right now, and a backup might be empty or out of date. Try again once the storage is back (restarting the hub may help).</p>
        <p className="mt-1 text-muted-foreground">The hub said: {e.message}</p>
      </Alert>
    )
  return <ErrorState error={error} what="Downloading backups" />
}

/** A 422 whose detail is `unsupported_schema`: the file is from newer firmware (or one this firmware can't migrate). */
const unsupportedSchema = (e: unknown) => {
  const err = toApiError(e)
  return err.status === 422 && err.details.some((d) => d.code === 'unsupported_schema')
}

function RestoreError({ error }: { error: unknown }) {
  if (unsupportedSchema(error))
    return (
      <Alert variant="error" title="This file needs newer firmware">
        <p>This file was made by newer firmware; update the hub first. Nothing was changed.</p>
        <p className="mt-1 text-muted-foreground">The hub said: {toApiError(error).message}</p>
      </Alert>
    )
  return <ErrorState error={error} what="Restoring backups" />
}

function saveBlob(blob: Blob, filename: string) {
  const url = URL.createObjectURL(blob)
  const a = Object.assign(document.createElement('a'), { href: url, download: filename })
  document.body.append(a)
  a.click()
  a.remove()
  setTimeout(() => URL.revokeObjectURL(url), 0)
}

function Backup() {
  const blocked = useConfigLock().state === 'blocked'
  const [file, setFile] = useState<{ name: string; text: string } | null>(null)
  const [preview, setPreview] = useState<RestoreResult | null>(null)
  const [error, setError] = useState<unknown>(null)
  const [downloaded, setDownloaded] = useState<{ kind: BackupKind; filename: string } | null>(null)
  const [downloadError, setDownloadError] = useState<unknown>(null)
  const [busy, setBusy] = useState<'download' | 'check' | 'restore' | null>(null)

  const download = async () => {
    setBusy('download')
    setDownloaded(null)
    setDownloadError(null)
    try {
      const d = await api.downloadBackup()
      saveBlob(d.blob, d.filename)
      setDownloaded({ kind: d.kind, filename: d.filename })
      if (d.kind === 'config') toast('Backup downloaded', { variant: 'success', description: d.filename })
    } catch (e) {
      setDownloadError(e)
    } finally {
      setBusy(null)
    }
  }

  const pick = async (f: File | undefined) => {
    setPreview(null)
    setError(null)
    if (!f) return setFile(null)
    try {
      const text = await f.text()
      JSON.parse(text) // early feedback only: the hub gets the file exactly as chosen
      setFile({ name: f.name, text })
      setBusy('check')
      setPreview(await api.restore(text, true))
    } catch (e) {
      setError(e instanceof SyntaxError ? new Error('That file isn’t valid JSON.') : e)
    } finally {
      setBusy(null)
    }
  }

  const restore = async () => {
    if (!file) return
    setBusy('restore')
    try {
      await api.restore(file.text, false)
      invalidateAll()
      toast('Backup restored', { variant: 'success' })
      setFile(null)
      setPreview(null)
    } catch (e) {
      setError(e)
    } finally {
      setBusy(null)
    }
  }

  const showRawNote = blocked || downloaded?.kind === 'blocked-raw'
  return (
    <div className="grid gap-4">
      <Section
        title={blocked ? 'Download the saved configuration (raw)' : 'Download a backup'}
        description={
          blocked
            ? 'The hub can’t run its saved configuration, so there’s no normal backup. You can still download the saved configuration as stored, to keep it before restoring or resetting.'
            : 'One JSON file with devices, activities and integration settings. Passwords, API tokens, Home Assistant tokens and Bluetooth bonds are never included.'
        }
      >
        <div className="grid gap-4">
          {showRawNote && (
            <Alert variant="warning" title={downloaded?.kind === 'blocked-raw' ? `Saved as ${downloaded.filename}` : 'Raw saved configuration'}>
              <p>{BLOCKED_RAW_NOTE}</p>
            </Alert>
          )}
          {downloadError != null && <DownloadError error={downloadError} />}
          <div>
            <Button variant="outline" onClick={download} disabled={!!busy}>
              {busy === 'download' ? <Loader2 className="animate-spin" /> : <Download />} {blocked ? 'Download the saved configuration (raw)' : 'Download backup'}
            </Button>
          </div>
        </div>
      </Section>
      <Section title="Restore" description="The hub validates the whole file before replacing anything. Restoring never starts an activity: one that’s running stays on if the backup still has it, otherwise nothing is active.">
        <div className="grid gap-4">
          <label className="flex cursor-pointer flex-col items-center gap-2 rounded-lg border border-dashed px-4 py-8 text-center text-sm hover:bg-accent/40 focus-within:ring-[3px] focus-within:ring-ring/50">
            <Upload className="size-5 text-muted-foreground" aria-hidden />
            <span>{file ? file.name : 'Choose a backup file'}</span>
            <input type="file" accept="application/json,.json" className="sr-only" onChange={(e) => pick(e.target.files?.[0])} />
          </label>
          {busy === 'check' && <p className="text-sm text-muted-foreground">Checking…</p>}
          {error != null && <RestoreError error={error} />}
          {preview && (
            <Alert variant="warning" title="Ready to restore">
              <p>
                Replaces all devices and activities with {preview.devices} devices and {preview.activities} activities.
                {preview.requires.length > 0 && <> Afterwards: {preview.requires.map((r) => REQUIRES[r]).join(', ')}.</>}
              </p>
            </Alert>
          )}
          <div>
            <Button variant="destructive" disabled={!preview || !!busy} onClick={restore}>
              {busy === 'restore' && <Loader2 className="animate-spin" />} Restore
            </Button>
          </div>
        </div>
      </Section>
      <FactoryReset />
    </div>
  )
}

function FactoryReset() {
  const [confirm, setConfirm] = useState(false)
  const [busy, setBusy] = useState(false)
  const reset = async () => {
    setBusy(true)
    try {
      await api.factoryReset()
      invalidateAll() // every object has a new revision now
      toast('Reset to factory defaults', { variant: 'success', description: 'Devices, activities and global buttons were removed. Your password and API tokens are unchanged.' })
      setConfirm(false)
    } catch (e) {
      toastError('Factory reset failed', e)
    } finally {
      setBusy(false)
    }
  }
  return (
    <Section title="Factory reset" description="Removes every device, activity and global button and resets integration settings. Your admin password, API tokens and pairings are kept. Works even when the saved configuration can’t be used.">
      <Button variant="outline" className="text-destructive hover:text-destructive" onClick={() => setConfirm(true)}>
        <RotateCcw /> Factory reset…
      </Button>
      <Dialog
        open={confirm}
        onClose={() => setConfirm(false)}
        title="Reset to factory defaults?"
        description="This can’t be undone. Download a backup first if you might want this configuration again."
        footer={
          <>
            <Button variant="ghost" onClick={() => setConfirm(false)}>
              Cancel
            </Button>
            <Button variant="destructive" onClick={reset} disabled={busy}>
              {busy && <Loader2 className="animate-spin" />} Reset everything
            </Button>
          </>
        }
      />
    </Section>
  )
}

function Firmware() {
  const fw = useQuery('firmware', () => api.firmware())
  const [file, setFile] = useState<File | null>(null)
  const [busy, setBusy] = useState(false)
  const [result, setResult] = useState<{ ok: true; message: string } | { ok: false; error: unknown } | null>(null)

  const upload = async () => {
    if (!file) return
    setBusy(true)
    setResult(null)
    try {
      const r = await api.uploadFirmware(file)
      const message = r.message ?? (r.update_state === 'rebooting' ? 'Verified. The hub is rebooting into the new version.' : `Update state: ${r.update_state}`)
      setResult(r.update_state === 'failed' ? { ok: false, error: new Error(message) } : { ok: true, message })
    } catch (e) {
      setResult({ ok: false, error: e })
    } finally {
      setBusy(false)
    }
  }

  return (
    <Section title="Firmware" description="Updates are verified for this board before flashing, and the hub rolls back if the new version fails to boot.">
      <div className="grid gap-5">
        {fw.data ? (
          <dl className="grid grid-cols-2 gap-x-6 gap-y-2 text-sm sm:grid-cols-4">
            {[
              ['Version', fw.data.version],
              ['Board', fw.data.board],
              ['Running slot', fw.data.running_partition ?? '–'],
              ['Rollback', fw.data.rollback_available ? 'Available' : 'None'],
            ].map(([k, v]) => (
              <div key={k}>
                <dt className="text-xs text-muted-foreground">{k}</dt>
                <dd className="truncate font-medium">{v}</dd>
              </div>
            ))}
          </dl>
        ) : fw.error ? (
          <ErrorState error={fw.error} retry={fw.refetch} what="Firmware updates" />
        ) : (
          <Skeleton className="h-10" />
        )}
        <div className="flex flex-wrap items-center gap-3">
          <label className="inline-flex">
            <span className="sr-only">Firmware image</span>
            <Input type="file" accept=".bin" onChange={(e) => setFile(e.target.files?.[0] ?? null)} className="max-w-xs" />
          </label>
          <Button onClick={upload} disabled={!file || busy}>
            {busy ? <Loader2 className="animate-spin" /> : <Upload />} Install update
          </Button>
        </div>
        <p className="text-xs text-muted-foreground">Remote response may pause briefly while the image is written.</p>
        {result && (result.ok ? <Alert variant="success">{result.message}</Alert> : <ErrorState error={result.error} what="Firmware updates" />)}
      </div>
    </Section>
  )
}

function Diagnostics() {
  const status = useStatus()
  const d = status.data?.diagnostics
  const r = status.data?.remote
  return (
    <div className="grid gap-4">
      <Section title="Recent errors" description="The hub keeps the last 16. Secrets are never logged.">
        {!d ? (
          <Skeleton className="h-20" />
        ) : !d.last_errors?.length ? (
          <p className="text-sm text-muted-foreground">No errors since boot.</p>
        ) : (
          <pre className="max-h-96 overflow-auto rounded-lg bg-muted p-3 font-mono text-xs leading-relaxed">
            {d.last_errors.map((e) => `${e.at ?? '-'}  ${(e.source ?? '').padEnd(10)} ${e.code.padEnd(12)} ${e.message}`).join('\n')}
          </pre>
        )}
      </Section>
      {r && (
        <Section title="Radio">
          <dl className="grid grid-cols-2 gap-x-6 gap-y-3 text-sm sm:grid-cols-4">
            {[
              ['Channel', r.channel ?? '–'],
              ['Last packet', agoMs(r.last_packet_age_ms)],
              ['Bad checksums', r.counters?.bad_checksum ?? 0],
              ['Unknown codes', r.counters?.unknown_code ?? 0],
            ].map(([k, v]) => (
              <div key={String(k)}>
                <dt className="text-xs text-muted-foreground">{k}</dt>
                <dd className="font-medium tabular-nums">{v}</dd>
              </div>
            ))}
          </dl>
        </Section>
      )}
      <Section title="Raw status" description="What /api/v1/status returns, for bug reports.">
        <div className="mb-3 flex justify-end">
          <Button variant="outline" size="sm" onClick={() => status.refetch()}>
            <RefreshCw /> Refresh
          </Button>
        </div>
        <pre className="max-h-96 overflow-auto rounded-lg bg-muted p-3 font-mono text-xs">{status.data ? JSON.stringify(status.data, null, 2) : 'Loading…'}</pre>
      </Section>
    </div>
  )
}
