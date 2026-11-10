import { DatabaseZap, ShieldAlert } from 'lucide-react'
import { Link } from 'wouter'
import { buttonVariants } from '@/components/ui/button'
import { useConfigLock } from '@/hooks/data'

/** Persistent while the saved configuration can't be used or written. */
export function ConfigBanner({ admin, link = true }: { admin: boolean; /** Off inside the setup wizard, which can't navigate to Settings yet. */ link?: boolean }) {
  const { state, reason } = useConfigLock()
  if (state !== 'blocked' && state !== 'storage_unavailable') return null
  const blocked = state === 'blocked'
  const Icon = blocked ? ShieldAlert : DatabaseZap
  return (
    <div role="alert" className="border-b border-destructive/30 bg-destructive/10 px-4 py-3 md:px-8">
      <div className="mx-auto flex max-w-6xl flex-wrap items-start gap-3">
        <Icon className="mt-0.5 size-5 shrink-0 text-destructive" aria-hidden />
        <div className="grid min-w-0 flex-1 gap-1 text-sm">
          <p className="font-semibold">{blocked ? 'The saved configuration can’t be used' : 'The hub can’t save changes'}</p>
          <p className="text-muted-foreground">
            {blocked
              ? 'Hubbo found a saved configuration it can’t run, so nothing is active and changes are refused. The saved data is left untouched.'
              : 'The hub’s storage isn’t writable, so changes are refused until it’s fixed.'}
            {reason && <> Reason: <span className="text-foreground">{reason}</span>.</>}
          </p>
          <p className="text-muted-foreground">
            {blocked
              ? admin
                ? link
                  ? 'To fix it, download the saved configuration (raw) to keep a copy, then restore a backup or reset the configuration.'
                  : 'Finish setup, then in Settings download the saved configuration (raw) to keep a copy, and restore a backup or reset the configuration.'
                : 'An admin needs to download the saved configuration (raw), then restore a backup or reset the configuration.'
              : 'Try restarting the hub. If this keeps happening, the storage may need to be wiped or the board may need attention.'}
          </p>
        </div>
        {blocked && admin && link && (
          <Link href="/settings/backup" className={buttonVariants({ size: 'sm', variant: 'outline' })}>
            Download raw, restore or reset…
          </Link>
        )}
      </div>
    </div>
  )
}
