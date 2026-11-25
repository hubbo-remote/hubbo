import { X } from 'lucide-react'
import { useEffect, useRef, type ReactNode } from 'react'
import { cn } from '@/lib/utils'

/**
 * Native <dialog>: focus trap, Esc and top-layer for free, no Radix weight. It opens and closes only
 * with `open`: Esc asks `onClose` at once (the browser's own close is held back, and its close event
 * comes later, maybe after other work), so the owner can refuse, e.g. while something runs.
 */
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
      onCancel={(e) => {
        // Only the dialog's own (Esc): a file input's cancel (its picker dismissed) bubbles up here too.
        if (e.target !== e.currentTarget) return
        e.preventDefault()
        onClose()
      }}
      // The browser can still close it itself (a repeated Esc it won't let a page refuse): tell the owner.
      onClose={() => open && onClose()}
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
