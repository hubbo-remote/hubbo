import { useId, useRef, type ReactNode } from 'react'
import { cn } from '@/lib/utils'

export interface TabDef {
  id: string
  label: ReactNode
}

/** WAI-ARIA tabs with arrow-key roving focus. */
export function Tabs({ tabs, value, onChange, className, children }: { tabs: TabDef[]; value: string; onChange: (id: string) => void; className?: string; children: ReactNode }) {
  const base = useId()
  const refs = useRef<(HTMLButtonElement | null)[]>([])
  const idx = tabs.findIndex((t) => t.id === value)
  const move = (d: number) => {
    const n = (idx + d + tabs.length) % tabs.length
    onChange(tabs[n]!.id)
    refs.current[n]?.focus()
  }
  return (
    <div className={className}>
      <div role="tablist" className="inline-flex h-9 max-w-full items-center gap-1 overflow-x-auto rounded-lg bg-muted p-1 text-muted-foreground">
        {tabs.map((t, i) => (
          <button
            key={t.id}
            ref={(el) => {
              refs.current[i] = el
            }}
            role="tab"
            id={`${base}-${t.id}`}
            aria-selected={t.id === value}
            aria-controls={`${base}-panel`}
            tabIndex={t.id === value ? 0 : -1}
            onClick={() => onChange(t.id)}
            onKeyDown={(e) => {
              if (e.key === 'ArrowRight') move(1)
              if (e.key === 'ArrowLeft') move(-1)
            }}
            className={cn(
              'inline-flex h-7 items-center gap-1.5 rounded-md px-3 text-sm font-medium whitespace-nowrap transition-colors outline-none focus-visible:ring-[3px] focus-visible:ring-ring',
              t.id === value ? 'bg-card text-foreground shadow-sm' : 'hover:text-foreground',
            )}
          >
            {t.label}
          </button>
        ))}
      </div>
      <div role="tabpanel" id={`${base}-panel`} aria-labelledby={`${base}-${value}`} className="mt-4 outline-none">
        {children}
      </div>
    </div>
  )
}
