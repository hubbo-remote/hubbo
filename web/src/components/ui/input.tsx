import { ChevronDown } from 'lucide-react'
import { createContext, useContext, type ComponentProps } from 'react'
import { cn } from '@/lib/utils'
import { InfoTip } from './info-tip'

const field =
  'flex h-9 w-full min-w-0 rounded-md border border-input bg-card px-3 py-1 text-base shadow-xs transition-[color,box-shadow] outline-none placeholder:text-muted-foreground focus-visible:border-ring focus-visible:ring-[3px] focus-visible:ring-ring/50 disabled:cursor-not-allowed disabled:opacity-50 aria-invalid:border-destructive aria-invalid:ring-destructive/20 md:text-sm'

/**
 * Whether fields paint their errors. A form or editor provides `false` until the first save (or
 * submit) attempt, so people can fill things in without being told off; after that errors update
 * live. Outside a provider errors show (e.g. ones the hub returned for a submit).
 */
export const ShowErrors = createContext(true)
const useShowErrors = () => useContext(ShowErrors)

/** The sign-in fields: the only ones a password manager should fill or offer to save. */
const CREDENTIALS = new Set(['username', 'current-password', 'new-password'])
/** Everything else (names, the HA token, addresses) asks password managers to leave it alone. */
const NOT_A_LOGIN = { autoComplete: 'off', 'data-1p-ignore': '', 'data-lpignore': 'true', 'data-form-type': 'other' }

export function Input({ className, 'aria-invalid': invalid, ...props }: ComponentProps<'input'>) {
  const show = useShowErrors()
  const login = CREDENTIALS.has(props.autoComplete ?? '')
  return <input className={cn(field, 'file:border-0 file:bg-transparent file:text-sm file:font-medium', className)} aria-invalid={show ? invalid : undefined} {...(login ? {} : NOT_A_LOGIN)} {...props} />
}

/** A multi-line field, in monospace (for JSON and the like). */
export function Textarea({ className, 'aria-invalid': invalid, ...props }: ComponentProps<'textarea'>) {
  const show = useShowErrors()
  return <textarea className={cn(field, 'h-auto py-2 font-mono', className)} aria-invalid={show ? invalid : undefined} spellCheck={false} autoCapitalize="none" {...NOT_A_LOGIN} {...props} />
}

/**
 * Native select styled like shadcn's: accessible, zero JS, good on phones. `appearance-none` drops
 * the platform's arrow, so a real chevron icon marks it as a picker (iOS Safari too).
 */
export function Select({ className, 'aria-invalid': invalid, ...props }: ComponentProps<'select'>) {
  const show = useShowErrors()
  return (
    <div className="relative">
      <select className={cn(field, 'peer appearance-none pr-8', className)} aria-invalid={show ? invalid : undefined} {...props} />
      <ChevronDown aria-hidden className="pointer-events-none absolute top-1/2 right-2.5 size-4 -translate-y-1/2 text-muted-foreground peer-disabled:opacity-50" />
    </div>
  )
}

export function Label({ className, ...props }: ComponentProps<'label'>) {
  return <label className={cn('text-sm font-medium leading-none select-none', className)} {...props} />
}

/**
 * `inline`: a control in a row, with no hint; its label is for screen readers only, or with
 * `'beside'` a short one before the control (for a number field, whose value doesn't say what it is).
 * `info`: a description shown as a tooltip by the label (beside the control when the label is hidden).
 */
export function Field({ label, hint, error, children, htmlFor, inline, info }: { label: string; hint?: React.ReactNode; error?: string | null; children: React.ReactNode; htmlFor: string; inline?: boolean | 'beside'; info?: string }) {
  const show = useShowErrors()
  const tip = info ? <InfoTip text={info} label={label} /> : null
  return (
    <div className="grid gap-1.5">
      {inline === 'beside' ? (
        <div className="flex items-center gap-2">
          <span className="flex items-center gap-1">
            <Label htmlFor={htmlFor} className="text-xs font-normal whitespace-nowrap text-muted-foreground">
              {label}
            </Label>
            {tip}
          </span>
          {children}
        </div>
      ) : inline ? (
        <div className="flex items-center gap-1.5">
          <Label htmlFor={htmlFor} className="sr-only">
            {label}
          </Label>
          {children}
          {tip}
        </div>
      ) : (
        <>
          <span className="flex items-center gap-1.5">
            <Label htmlFor={htmlFor}>{label}</Label>
            {tip}
          </span>
          {children}
        </>
      )}
      {error && show ? (
        <p className="text-xs text-destructive" role="alert">
          {error}
        </p>
      ) : hint && !inline ? (
        <p className="text-xs text-muted-foreground">{hint}</p>
      ) : null}
    </div>
  )
}
