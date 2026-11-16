import { Check, type LucideIcon } from 'lucide-react'
import { useEffect, useRef, type ComponentProps, type KeyboardEvent, type ReactNode, type RefObject } from 'react'
import { cn } from '@/lib/utils'

/**
 * A ⋯ menu: `trigger` (a button that toggles `open`) and its items below it. It closes on a click or
 * tap outside, on Esc (focus back on the trigger), on Tab, and once an item is chosen. Arrows, Home
 * and End move between items. On open the checked item, or else the first, takes focus (lit only
 * for the keyboard), so callers don't autofocus anything.
 */
export function Menu({ label, open, onOpenChange, trigger, children, className }: { label: string; open: boolean; onOpenChange: (open: boolean) => void; /** Renders the button, spreading these props onto it. */ trigger: (props: MenuTriggerProps) => ReactNode; children: ReactNode; className?: string }) {
  const root = useRef<HTMLDivElement>(null)
  const button = useRef<HTMLButtonElement>(null)
  const list = useRef<HTMLDivElement>(null)
  // Held in a ref so the outside-press listener subscribes once per opening, not on every render.
  const change = useRef(onOpenChange)
  useEffect(() => void (change.current = onOpenChange))
  useEffect(() => {
    if (!open) return
    const items = list.current
    ;(items?.querySelector<HTMLElement>('[aria-checked="true"]') ?? items?.querySelector<HTMLElement>(ITEM))?.focus()
    const outside = (e: PointerEvent) => root.current && !root.current.contains(e.target as Node) && change.current(false)
    document.addEventListener('pointerdown', outside)
    return () => document.removeEventListener('pointerdown', outside)
  }, [open])

  const keys = (e: KeyboardEvent<HTMLDivElement>) => {
    const items = [...e.currentTarget.querySelectorAll<HTMLElement>(ITEM)].filter((el) => !(el as HTMLButtonElement).disabled)
    const i = items.indexOf(document.activeElement as HTMLElement)
    const go = (j: number) => (e.preventDefault(), items[(j + items.length) % items.length]?.focus())
    if (e.key === 'ArrowDown') go(i + 1)
    else if (e.key === 'ArrowUp') go(i - 1)
    else if (e.key === 'Home') go(0)
    else if (e.key === 'End') go(items.length - 1)
    else if (e.key === 'Tab') onOpenChange(false)
    else if (e.key === 'Escape') {
      e.preventDefault()
      onOpenChange(false)
      button.current?.focus()
    }
  }

  return (
    // Open, the root itself rises: a caller's transform (or z-index) makes it a stacking context, and then
    // the list's own z-index only counts inside it, under whatever comes later on the page. z-20 beats later
    // tiles and stays under the sticky header and bottom nav (z-30).
    <div ref={root} className={cn('relative', open && 'z-20', className)}>
      {trigger({ ref: button, 'aria-haspopup': 'menu', 'aria-expanded': open, onClick: () => onOpenChange(!open) })}
      {open && (
        <div
          ref={list}
          role="menu"
          aria-label={label}
          className="absolute top-full right-0 z-20 mt-1 grid w-40 rounded-md border bg-popover p-1 text-popover-foreground shadow-lg"
          onKeyDown={keys}
          onClick={(e) => (e.target as Element).closest(ITEM) && onOpenChange(false)}
        >
          {children}
        </div>
      )}
    </div>
  )
}

export interface MenuTriggerProps {
  ref: RefObject<HTMLButtonElement | null>
  'aria-haspopup': 'menu'
  'aria-expanded': boolean
  onClick: () => void
}

const ITEM = '[role="menuitem"], [role="menuitemradio"]'

/**
 * A menu row: highlighted on hover, or on keyboard focus only (`focus-visible`), so the item a click
 * opens on isn't lit up. Icons sit in one muted column; a destructive item is red throughout.
 */
export function MenuItem({ icon: Icon, destructive, checked, children, ...props }: { icon: LucideIcon; destructive?: boolean; /** A radio-style choice: checked or not. */ checked?: boolean } & ComponentProps<'button'>) {
  const radio = checked !== undefined
  return (
    <button
      role={radio ? 'menuitemradio' : 'menuitem'}
      aria-checked={radio ? checked : undefined}
      className={cn(
        'flex h-8 w-full items-center gap-2 rounded-sm px-2 text-left text-sm outline-none hover:bg-accent focus-visible:bg-accent disabled:pointer-events-none disabled:opacity-50',
        destructive && 'text-destructive hover:bg-destructive/10 focus-visible:bg-destructive/10',
      )}
      {...props}
    >
      <Icon className={cn('size-4 shrink-0', !destructive && 'text-muted-foreground')} aria-hidden />
      {children}
      {checked && <Check className="ml-auto size-4 shrink-0" aria-hidden />}
    </button>
  )
}

/** A line between groups, e.g. before a destructive item. */
export const MenuSeparator = () => <div role="separator" className="-mx-1 my-1 h-px bg-border" />
