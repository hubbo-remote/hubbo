import { DatabaseZap, RefreshCw } from 'lucide-react'
import { ThemeToggle } from '@/components/AppShell'
import { Logo } from '@/components/Logo'
import { Button } from '@/components/ui/button'

/**
 * Shown instead of the first-run wizard or the login form when the hub can't
 * read its saved login data (`auth_state: unavailable`, read once at boot).
 * Setup is never offered: "can't read" is not "doesn't exist".
 */
export function CredentialsProblem({ onRetry }: { onRetry: () => void }) {
  return (
    <div className="grid min-h-dvh place-items-center bg-muted/40 p-4">
      <div className="fixed top-4 right-4">
        <ThemeToggle />
      </div>
      <div role="alert" className="grid w-full max-w-md gap-5 rounded-2xl border bg-card p-7 shadow-sm">
        <div className="flex items-center gap-3">
          <Logo className="size-9" />
          <DatabaseZap className="size-6 text-destructive" aria-hidden />
        </div>
        <div className="grid gap-2">
          <h1 className="text-xl font-semibold">Hubbo can’t read its login data</h1>
          <p className="text-sm text-muted-foreground">The hub couldn’t read its saved admin account when it started. Nothing has been changed, and sign-in and setup are unavailable.</p>
          <p className="text-sm text-muted-foreground">Restart the hub. If this keeps happening, wipe it and set it up again (then restore a backup).</p>
        </div>
        <Button variant="outline" onClick={onRetry}>
          <RefreshCw /> Check again
        </Button>
      </div>
    </div>
  )
}
