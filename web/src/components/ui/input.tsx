import type { ComponentProps } from 'react'
import { cn } from '@/lib/utils'

const field =
  'flex h-9 w-full min-w-0 rounded-md border border-input bg-card px-3 py-1 text-base shadow-xs transition-[color,box-shadow] outline-none placeholder:text-muted-foreground focus-visible:border-ring focus-visible:ring-[3px] focus-visible:ring-ring/50 disabled:cursor-not-allowed disabled:opacity-50 aria-invalid:border-destructive aria-invalid:ring-destructive/20 md:text-sm'

export function Input({ className, ...props }: ComponentProps<'input'>) {
  return <input className={cn(field, 'file:border-0 file:bg-transparent file:text-sm file:font-medium', className)} {...props} />
}

/** Native select styled like shadcn's: accessible, zero JS, good on phones. */
export function Select({ className, ...props }: ComponentProps<'select'>) {
  return (
    <select
      className={cn(
        field,
        'appearance-none bg-[length:1rem] bg-[right_0.6rem_center] bg-no-repeat pr-8',
        "bg-[url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23888' stroke-width='2'%3E%3Cpath d='m6 9 6 6 6-6'/%3E%3C/svg%3E\")]",
        className,
      )}
      {...props}
    />
  )
}

export function Label({ className, ...props }: ComponentProps<'label'>) {
  return <label className={cn('text-sm font-medium leading-none select-none', className)} {...props} />
}

export function Field({ label, hint, error, children, htmlFor }: { label: string; hint?: React.ReactNode; error?: string | null; children: React.ReactNode; htmlFor: string }) {
  return (
    <div className="grid gap-1.5">
      <Label htmlFor={htmlFor}>{label}</Label>
      {children}
      {error ? (
        <p className="text-xs text-destructive" role="alert">
          {error}
        </p>
      ) : hint ? (
        <p className="text-xs text-muted-foreground">{hint}</p>
      ) : null}
    </div>
  )
}
