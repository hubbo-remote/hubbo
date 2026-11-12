import { Info } from 'lucide-react'
import { useEffect, useId, useRef, useState } from 'react'

/**
 * A small info icon whose text shows on hover, focus or tap (never inline). Esc or moving away
 * closes it. Fixed to the viewport so a dialog's or row's overflow can't clip it.
 */
export function InfoTip({ text, label }: { text: string; label: string }) {
  const id = useId()
  const ref = useRef<HTMLButtonElement>(null)
  const [at, setAt] = useState<{ top: number; left: number } | null>(null)
  const show = () => {
    const r = ref.current?.getBoundingClientRect()
    if (r) setAt({ top: r.bottom + 6, left: Math.max(8, Math.min(r.left - 8, window.innerWidth - 8 - Math.min(256, window.innerWidth - 16))) })
  }
  const hide = () => setAt(null)
  useEffect(() => {
    if (!at) return
    window.addEventListener('scroll', hide, true)
    return () => window.removeEventListener('scroll', hide, true)
  }, [at])
  return (
    <>
      <button
        ref={ref}
        type="button"
        aria-label={`About ${label}`}
        aria-describedby={id}
        onMouseEnter={show}
        onMouseLeave={hide}
        onFocus={show}
        onBlur={hide}
        onClick={show}
        onKeyDown={(e) => {
          if (e.key === 'Escape' && at) {
            e.preventDefault() // close the tip, not the dialog around it
            e.stopPropagation()
            hide()
          }
        }}
        className="inline-flex shrink-0 rounded-full text-muted-foreground/70 outline-none hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring"
      >
        <Info className="size-3.5" aria-hidden />
      </button>
      <span
        id={id}
        role="tooltip"
        hidden={!at}
        style={at ? { top: at.top, left: at.left } : undefined}
        className="fixed z-50 w-max max-w-[min(16rem,calc(100vw-1rem))] rounded-md border bg-popover px-2.5 py-1.5 text-xs font-normal text-popover-foreground shadow-md"
      >
        {text}
      </span>
    </>
  )
}
