import { DatabaseZap, RefreshCw, ShieldX } from 'lucide-react'
import { ThemeToggle } from '@/components/AppShell'
import { Logo } from '@/components/Logo'
import { Button } from '@/components/ui/button'

/**
 * Shown instead of the first-run wizard or the login form when the hub can't
 * read (`unavailable`) or trust (`corrupt`) its saved login data. Setup is
 * never offered: "can't read" is not "doesn't exist".
 */
export function CredentialsProblem({ kind, onRetry }: { kind: 'unavailable' | 'corrupt'; onRetry: () => void }) {
  const unavailable = kind === 'unavailable'
  const Icon = unavailable ? DatabaseZap : ShieldX
  return (
    <div className="grid min-h-dvh place-items-center bg-muted/40 p-4">
      <div className="fixed top-4 right-4">
        <ThemeToggle />
      </div>
      <div role="alert" className="grid w-full max-w-md gap-5 rounded-2xl border bg-card p-7 shadow-sm">
        <div className="flex items-center gap-3">
          <Logo className="size-9" />
          <Icon className="size-6 text-destructive" aria-hidden />
        </div>
        {unavailable ? (
          <div className="grid gap-2">
            <h1 className="text-xl font-semibold">Hubbo can’t read its storage</h1>
            <p className="text-sm text-muted-foreground">The hub can’t read its saved login data. Nothing has been changed. Check the device’s storage; it’ll recover when storage is readable.</p>
            <p className="text-sm text-muted-foreground">Sign-in and setup are unavailable until then.</p>
          </div>
        ) : (
          <div className="grid gap-2">
            <h1 className="text-xl font-semibold">The saved login data is damaged</h1>
            <p className="text-sm text-muted-foreground">The hub’s admin account data can’t be trusted, so no one can sign in. Your devices and activities are not affected.</p>
            <p className="text-sm text-muted-foreground">Recovering needs physical access to the hub: reset its credentials from the device itself (see the hardware guide). Setting up a new admin from here isn’t possible.</p>
          </div>
        )}
        <Button variant="outline" onClick={onRetry}>
          <RefreshCw /> Check again
        </Button>
      </div>
    </div>
  )
}
