import { CheckCircle2, XCircle } from 'lucide-react'
import { useSyncExternalStore } from 'react'
import { errorMessage } from '@/api/client'
import { cn } from '@/lib/utils'

interface Toast {
  id: number
  title: string
  description?: string
  variant: 'success' | 'error' | 'default'
}

let toasts: Toast[] = []
let seq = 0
const listeners = new Set<() => void>()
const emit = () => listeners.forEach((l) => l())

export function toast(title: string, opts: { description?: string; variant?: Toast['variant'] } = {}) {
  const t: Toast = { id: ++seq, title, description: opts.description, variant: opts.variant ?? 'default' }
  toasts = [...toasts.slice(-3), t]
  emit()
  setTimeout(() => {
    toasts = toasts.filter((x) => x.id !== t.id)
    emit()
  }, t.variant === 'error' ? 7000 : 3500)
}

export const toastError = (title: string, e: unknown): void => toast(title, { variant: 'error', description: errorMessage(e) })

export function Toaster() {
  const list = useSyncExternalStore(
    (l) => (listeners.add(l), () => listeners.delete(l)),
    () => toasts,
  )
  return (
    <div aria-live="polite" className="pointer-events-none fixed inset-x-0 bottom-0 z-50 flex flex-col items-center gap-2 p-4 sm:items-end">
      {list.map((t) => (
        <div key={t.id} className={cn('pointer-events-auto flex w-full max-w-sm gap-3 rounded-lg border bg-popover px-4 py-3 text-sm shadow-lg', t.variant === 'error' && 'border-destructive/40')}>
          {t.variant === 'success' && <CheckCircle2 className="mt-0.5 size-4 shrink-0 text-success" aria-hidden />}
          {t.variant === 'error' && <XCircle className="mt-0.5 size-4 shrink-0 text-destructive" aria-hidden />}
          <div className="grid gap-0.5">
            <p className="font-medium">{t.title}</p>
            {t.description && <p className="text-muted-foreground">{t.description}</p>}
          </div>
        </div>
      ))}
    </div>
  )
}
