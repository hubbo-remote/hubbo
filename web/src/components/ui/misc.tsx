import type { ComponentProps } from 'react'
import { cn } from '@/lib/utils'

export function Skeleton({ className, ...props }: ComponentProps<'div'>) {
  return <div className={cn('animate-pulse rounded-md bg-muted', className)} {...props} />
}

export function Spinner({ className }: { className?: string }) {
  return <span role="status" aria-label="Loading" className={cn('inline-block size-4 animate-spin rounded-full border-2 border-current border-r-transparent', className)} />
}

export function Kbd({ className, ...props }: ComponentProps<'kbd'>) {
  return <kbd className={cn('rounded border bg-muted px-1.5 py-0.5 font-mono text-[11px] text-muted-foreground', className)} {...props} />
}
