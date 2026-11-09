import { ArrowLeft, Construction, type LucideIcon } from 'lucide-react'
import { Link } from 'wouter'
import type { ReactNode } from 'react'
import type { IntegrationState } from '@/api/types'
import { HEALTH } from '@/lib/health'
import { ApiError, errorMessage, isNotImplemented } from '@/api/client'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from '@/components/ui/card'
import { cn } from '@/lib/utils'

export function PageHeader({ title, description, actions, back }: { title: ReactNode; description?: ReactNode; actions?: ReactNode; back?: { href: string; label: string } }) {
  return (
    <header className="mb-6 flex flex-wrap items-end justify-between gap-3">
      <div className="grid min-w-0 gap-1">
        {back && (
          <Link href={back.href} className="mb-1 inline-flex items-center gap-1 text-sm text-muted-foreground hover:text-foreground">
            <ArrowLeft className="size-4" /> {back.label}
          </Link>
        )}
        <h1 className="truncate text-2xl font-semibold tracking-tight">{title}</h1>
        {description && <p className="text-sm text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex flex-wrap items-center gap-2">{actions}</div>}
    </header>
  )
}

export function HealthDot({ state, className }: { state: IntegrationState; className?: string }) {
  const [c] = HEALTH[state]
  return (
    <span className={cn('relative inline-flex size-2.5', className)} aria-hidden>
      {state === 'connected' && <span className={cn('absolute inline-flex size-full rounded-full opacity-40 motion-safe:animate-ping', c)} style={{ animationDuration: '2.5s' }} />}
      <span className={cn('relative inline-flex size-2.5 rounded-full', c)} />
    </span>
  )
}


/** Calm placeholder for features the connected hub doesn't implement yet (HTTP 501). */
export function NotAvailable({ what = 'This feature' }: { what?: string }) {
  return (
    <div role="status" className="flex items-start gap-3 rounded-lg border border-dashed bg-muted/40 px-4 py-3 text-sm">
      <Construction className="mt-0.5 size-4 shrink-0 text-muted-foreground" aria-hidden />
      <div className="grid gap-0.5">
        <p className="font-medium">Not available yet</p>
        <p className="text-muted-foreground">
          {what}: this hub’s firmware doesn’t support it yet. Nothing is wrong with your setup.
        </p>
      </div>
    </div>
  )
}

/** A failed load or action: "not available yet" for 501s, otherwise an error alert (with Retry for loads). */
export function ErrorState({ error, retry, what }: { error: unknown; retry?: () => void; what?: string }) {
  if (isNotImplemented(error)) return <NotAvailable what={what} />
  const msg = errorMessage(error, { details: true })
  return (
    <Alert
      variant="error"
      title={error instanceof ApiError && error.status === 0 ? 'Cannot reach the hub' : 'Something went wrong'}
      action={retry && (
        <Button size="sm" variant="outline" onClick={retry}>
          Retry
        </Button>
      )}
    >
      <p className="whitespace-pre-line">{msg}</p>
    </Alert>
  )
}

export function EmptyState({ icon: Icon, title, children, action }: { icon: LucideIcon; title: string; children?: ReactNode; action?: ReactNode }) {
  return (
    <div className="flex flex-col items-center gap-3 rounded-xl border border-dashed px-6 py-12 text-center">
      <div className="rounded-full bg-muted p-3">
        <Icon className="size-5 text-muted-foreground" aria-hidden />
      </div>
      <div className="grid gap-1">
        <p className="font-medium">{title}</p>
        {children && <p className="max-w-sm text-sm text-muted-foreground">{children}</p>}
      </div>
      {action}
    </div>
  )
}

/** A titled card for settings-style forms. */
export function Section({ title, description, children, className = 'max-w-2xl' }: { title: string; description?: string; children: ReactNode; className?: string }) {
  return (
    <Card className={className}>
      <CardHeader>
        <div>
          <CardTitle>{title}</CardTitle>
          {description && <CardDescription className="mt-1">{description}</CardDescription>}
        </div>
      </CardHeader>
      <CardContent>{children}</CardContent>
    </Card>
  )
}
