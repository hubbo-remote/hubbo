import { Link } from 'wouter'
import { buttonVariants } from '@/components/ui/button'

export function NotFound() {
  return (
    <div className="grid place-items-center gap-3 py-24 text-center">
      <p className="text-5xl font-semibold text-muted-foreground/40">404</p>
      <p className="text-muted-foreground">That page doesn’t exist.</p>
      <Link href="/" className={buttonVariants({ variant: 'outline' })}>
        Back to dashboard
      </Link>
    </div>
  )
}
