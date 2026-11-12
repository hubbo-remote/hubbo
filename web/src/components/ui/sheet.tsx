import { useEffect, useRef, type ReactNode } from 'react'
import { Button } from './button'

/** The sheet's height; `SheetSpacer` gives the page the same room so content can scroll above it. */
const HEIGHT = 'h-[55dvh]'
const MAX_HEIGHT = 'max-h-[55dvh]'
/** The app's sticky phone header (h-14) plus a little air. */
const PHONE_HEADER = 64

/**
 * A phone's bottom sheet: about half the screen, scrolling inside, no scrim, so the page above stays
 * usable. Done or Esc closes it; no gestures. It takes focus on open and gives it back to the anchor
 * on close, and scrolls the anchor (`find(anchor)`) into view above it whenever the anchor changes.
 */
export function Sheet<A>({ label, anchor, find, onClose, children }: { label: string; anchor: A; find: (anchor: A) => HTMLElement | SVGElement | null; onClose: () => void; children: ReactNode }) {
  const ref = useRef<HTMLDivElement>(null)
  const current = useRef({ anchor, find })
  current.current = { anchor, find }
  useEffect(() => {
    ref.current?.focus({ preventScroll: true })
    return () => current.current.find(current.current.anchor)?.focus({ preventScroll: true })
  }, [])
  useEffect(() => {
    const top = ref.current?.getBoundingClientRect().top
    const r = current.current.find(anchor)?.getBoundingClientRect()
    if (!r || top === undefined) return
    if (r.bottom > top - 8) window.scrollBy({ top: r.bottom - top + 16, behavior: 'smooth' })
    else if (r.top < PHONE_HEADER) window.scrollBy({ top: r.top - PHONE_HEADER - 8, behavior: 'smooth' })
  }, [anchor])
  return (
    <div
      ref={ref}
      role="dialog"
      aria-label={label}
      tabIndex={-1}
      onKeyDown={(e) => {
        if (e.key === 'Escape' && !e.defaultPrevented) {
          e.preventDefault()
          onClose()
        }
      }}
      className={`fixed inset-x-0 bottom-0 z-40 ${MAX_HEIGHT} overflow-y-auto overscroll-contain rounded-t-2xl border-t bg-background px-3 pt-2 pb-[calc(env(safe-area-inset-bottom)+0.75rem)] shadow-[0_-8px_30px_oklch(0_0_0/0.18)] outline-none`}
    >
      <div className="sticky top-0 z-10 -mx-3 mb-2 flex justify-end bg-background/95 px-3 py-1 backdrop-blur">
        <Button size="sm" variant="ghost" onClick={onClose}>
          Done
        </Button>
      </div>
      {children}
    </div>
  )
}

/** Room at the end of the page, so its lowest content can scroll up above an open sheet. */
export const SheetSpacer = () => <div className={HEIGHT} aria-hidden />
