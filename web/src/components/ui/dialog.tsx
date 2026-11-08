import { X } from 'lucide-react'
import { useEffect, useRef, type ReactNode } from 'react'
import { cn } from '@/lib/utils'

/** Native <dialog>: focus trap, Esc and top-layer for free, no Radix weight. */
export function Dialog({ open, onClose, title, description, children, footer, className }: { open: boolean; onClose: () => void; title: string; description?: ReactNode; children?: ReactNode; footer?: ReactNode; className?: string }) {
  const ref = useRef<HTMLDialogElement>(null)
  useEffect(() => {
    const d = ref.current
    if (!d) return
    if (open && !d.open) d.showModal()
    if (!open && d.open) d.close()
  }, [open])
  return (
    <dialog
      ref={ref}
      onClose={onClose}
      onClick={(e) => e.target === ref.current && onClose()}
      aria-labelledby="dlg-title"
      className={cn(
        'm-auto w-[min(32rem,calc(100vw-2rem))] max-h-[calc(100dvh-2rem)] rounded-xl border bg-popover p-0 text-popover-foreground shadow-2xl backdrop:bg-black/40',
        className,
      )}
    >
      {open && (
        <div className="flex max-h-[calc(100dvh-2rem)] flex-col">
          <div className="flex items-start justify-between gap-4 border-b px-5 py-4">
            <div className="grid gap-1">
              <h2 id="dlg-title" className="text-lg font-semibold leading-none">
                {title}
              </h2>
              {description && <p className="text-sm text-muted-foreground">{description}</p>}
            </div>
            <button type="button" onClick={onClose} className="rounded-sm opacity-70 hover:opacity-100 focus-visible:ring-2 focus-visible:ring-ring outline-none" aria-label="Close">
              <X className="size-4" />
            </button>
          </div>
          <div className="overflow-y-auto px-5 py-4">{children}</div>
          {footer && <div className="flex flex-wrap justify-end gap-2 border-t px-5 py-3">{footer}</div>}
        </div>
      )}
    </dialog>
  )
}
