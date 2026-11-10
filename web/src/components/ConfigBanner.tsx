import { DatabaseZap, ShieldAlert } from 'lucide-react'
import { Link } from 'wouter'
import { buttonVariants } from '@/components/ui/button'
import { useConfigLock } from '@/hooks/data'

/** Persistent while the saved configuration can't be used or written. */
export function ConfigBanner({ admin, inSetup = false }: { admin: boolean; /** The wizard can't navigate yet, so no links. */ inSetup?: boolean }) {
  const { state } = useConfigLock()
  if (state !== 'blocked' && state !== 'storage_unavailable') return null
  const blocked = state === 'blocked'
  const Icon = blocked ? ShieldAlert : DatabaseZap
  return (
    <div role="alert" className="border-b border-destructive/30 bg-destructive/10 px-4 py-3 md:px-8">
      <div className="mx-auto flex max-w-6xl flex-wrap items-start gap-3">
        <Icon className="mt-0.5 size-5 shrink-0 text-destructive" aria-hidden />
        <div className="grid min-w-0 flex-1 gap-1 text-sm">
          <p className="font-semibold">{blocked ? 'Hubbo can’t use its saved configuration' : 'Hubbo can’t save changes'}</p>
          <p className="text-muted-foreground">
            {blocked
              ? !admin
                ? 'Changes are paused. An admin needs to restore a backup or reset the configuration.'
                : inSetup
                  ? 'Changes are paused. Finish setup, then restore a backup or reset the configuration.'
                  : 'Changes are paused. Download a copy, then restore a backup or reset the configuration.'
              : 'Changes are paused. Try restarting Hubbo.'}
          </p>
        </div>
        {blocked && admin && !inSetup && (
          <div className="flex flex-wrap gap-2">
            <Link href="/status/backups" className={buttonVariants({ size: 'sm', variant: 'outline' })}>
              Backups
            </Link>
            <Link href="/settings/reset" className={buttonVariants({ size: 'sm', variant: 'outline' })}>
              Reset
            </Link>
          </div>
        )}
      </div>
    </div>
  )
}
