import { Download, Loader2, Upload } from 'lucide-react'
import { useState } from 'react'
import { Link } from 'wouter'
import { api, toApiError, type BackupKind } from '@/api/client'
import { invalidateAll } from '@/api/hooks'
import type { RestoreResult } from '@/api/types'
import { ErrorState, Section } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button, buttonVariants } from '@/components/ui/button'
import { toast } from '@/components/ui/toast'
import { useConfigLock } from '@/hooks/data'
import { plural } from '@/lib/utils'

const REQUIRES: Record<RestoreResult['requires'][number], string> = {
  home_assistant_token: 'enter the Home Assistant token again',
  ble_pairing: 'pair Bluetooth again',
  api_tokens: 'create new API tokens',
}

/** Why a download failed (409 config_blocked, 503 storage_unavailable). */
function DownloadError({ error }: { error: unknown }) {
  const e = toApiError(error)
  if (e.code === 'config_blocked')
    return (
      <Alert variant="error" title="Nothing to download">
        The saved configuration is damaged. Restore a backup or reset the configuration.
      </Alert>
    )
  if (e.code === 'storage_unavailable')
    return (
      <Alert variant="error" title="Can’t read storage">
        Restart Hubbo and try again.
      </Alert>
    )
  return <ErrorState error={error} />
}

/** A 422 whose detail is `unsupported_schema`: the file is from newer firmware (or one this firmware can't migrate). */
const unsupportedSchema = (e: unknown) => {
  const err = toApiError(e)
  return err.status === 422 && err.details.some((d) => d.code === 'unsupported_schema')
}

function RestoreError({ error }: { error: unknown }) {
  if (unsupportedSchema(error))
    return (
      <Alert variant="error" title="This backup needs a newer Hubbo">
        Update Hubbo first. Nothing was changed.
      </Alert>
    )
  return <ErrorState error={error} />
}

function saveBlob(blob: Blob, filename: string) {
  const url = URL.createObjectURL(blob)
  const a = Object.assign(document.createElement('a'), { href: url, download: filename })
  document.body.append(a)
  a.click()
  a.remove()
  setTimeout(() => URL.revokeObjectURL(url), 0)
}

/** Download a backup (or, while blocked, the saved configuration as stored) and restore one. */
export function Backups() {
  const configState = useConfigLock().state
  const blocked = configState === 'blocked'
  const [file, setFile] = useState<{ name: string; text: string } | null>(null)
  const [preview, setPreview] = useState<RestoreResult | null>(null)
  const [error, setError] = useState<unknown>(null)
  const [downloaded, setDownloaded] = useState<{ kind: BackupKind; filename: string } | null>(null)
  const [downloadError, setDownloadError] = useState<unknown>(null)
  const [busy, setBusy] = useState<'download' | 'check' | 'restore' | null>(null)
  /** After a restore: Home Assistant has a URL but no token (backups never carry it). */
  const [haTokenMissing, setHaTokenMissing] = useState(false)

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
    setHaTokenMissing(false)
    if (!f) return setFile(null)
    try {
      const text = await f.text()
      JSON.parse(text) // early feedback only: the hub gets the file exactly as chosen
      setFile({ name: f.name, text })
      setBusy('check')
      setPreview(await api.restore(text, true))
    } catch (e) {
      setError(e instanceof SyntaxError ? new Error('That isn’t a backup file.') : e)
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
      const ha = await api.integrationConfig('ha_main').catch(() => null)
      setHaTokenMissing(ha?.settings.type === 'home_assistant' && !!ha.settings.url && !ha.settings.has_access_token)
    } catch (e) {
      setError(e)
    } finally {
      setBusy(null)
    }
  }

  const raw = blocked || downloaded?.kind === 'blocked-raw'
  return (
    <div className="grid gap-4">
      <Section
        title={blocked ? 'Download the saved configuration' : 'Download a backup'}
        description={blocked ? 'Keep a copy before you restore or reset.' : 'Devices, activities and settings. Passwords and tokens aren’t included.'}
      >
        <div className="grid gap-4">
          {configState === 'storage_unavailable' && <Alert variant="info" title="Backups still work while storage is down." />}
          {raw && (
            <Alert variant="warning" title={downloaded?.kind === 'blocked-raw' ? `Saved as ${downloaded.filename}` : undefined}>
              It may only restore on a newer Hubbo.
            </Alert>
          )}
          {downloadError != null && <DownloadError error={downloadError} />}
          <div>
            <Button variant="outline" onClick={download} disabled={!!busy}>
              {busy === 'download' ? <Loader2 className="animate-spin" /> : <Download />} {blocked ? 'Download as is' : 'Download backup'}
            </Button>
          </div>
        </div>
      </Section>
      <Section title="Restore" description="Replaces your devices, activities and settings.">
        <div className="grid gap-4">
          <label className="flex cursor-pointer flex-col items-center gap-2 rounded-lg border border-dashed px-4 py-8 text-center text-sm hover:bg-accent/40 focus-within:ring-[3px] focus-within:ring-ring/50">
            <Upload className="size-5 text-muted-foreground" aria-hidden />
            <span>{file ? file.name : 'Choose a backup file'}</span>
            <input type="file" accept="application/json,.json" className="sr-only" onChange={(e) => pick(e.target.files?.[0])} />
          </label>
          {busy === 'check' && <p className="text-sm text-muted-foreground">Checking…</p>}
          {haTokenMissing && (
            <Alert
              variant="warning"
              title="Enter the Home Assistant token again"
              action={
                <Link href="/integrations" className={buttonVariants({ size: 'sm', variant: 'outline' })}>
                  Integrations
                </Link>
              }
            />
          )}
          {error != null && <RestoreError error={error} />}
          {preview && (
            <Alert variant="warning" title="Ready to restore">
              <p>
                {plural(preview.devices, 'device')} and {plural(preview.activities, 'activity', 'activities')}.
                {preview.requires.length > 0 && <> Afterwards, {preview.requires.map((r) => REQUIRES[r]).join(', ')}.</>}
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
    </div>
  )
}
