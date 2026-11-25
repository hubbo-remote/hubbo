import type { LucideIcon } from 'lucide-react'

/** One row to pick in the Add device dialog. */
export function Choice({ icon: Icon, title, subtitle, disabled, onClick }: { icon?: LucideIcon; title: string; subtitle: string; disabled?: boolean; onClick: () => void }) {
  return (
    <button onClick={onClick} disabled={disabled} className="flex items-center gap-3 rounded-lg border p-3 text-left outline-none enabled:hover:bg-accent focus-visible:ring-[3px] focus-visible:ring-ring disabled:opacity-60">
      {Icon && <Icon className="size-5 shrink-0 text-muted-foreground" />}
      <div className="min-w-0">
        <p className="font-medium">{title}</p>
        <p className="text-xs text-muted-foreground">{subtitle}</p>
      </div>
    </button>
  )
}
