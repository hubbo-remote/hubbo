import { Activity as ActivityIcon, Copy, Globe, Loader2, MoreHorizontal, Pencil, Plus, Power, Trash2, type LucideIcon } from 'lucide-react'
import { useRef, useState, type ComponentProps, type FormEvent, type KeyboardEvent } from 'react'
import { Link, useLocation } from 'wouter'
import { api } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Activity } from '@/api/types'
import { EmptyState, ErrorState, PageHeader } from '@/components/common'
import { Button, buttonVariants } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input } from '@/components/ui/input'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { switchActivity } from '@/hooks/switchActivity'
import { useActivities, useConfigLock, useStatus } from '@/hooks/data'
import { LIMITS } from '@/lib/activity'
import { cn } from '@/lib/utils'

/** One column of large tap rows on a phone; compact rows in columns on wider screens. */
const GRID = 'grid gap-2 sm:grid-cols-2 lg:grid-cols-3'

/** The home page: switch activities with one tap. Editing sits behind each tile's small controls. */
export function Activities() {
  const acts = useActivities()
  const status = useStatus(2000)
  const [, navigate] = useLocation()
  const [pending, setPending] = useState<string | null>(null)
  const [confirm, setConfirm] = useState<Activity | null>(null)
  const [menu, setMenu] = useState<string | null>(null)
  const [dup, setDup] = useState<{ from: Activity; name: string; busy: boolean } | null>(null)
  // Dialog titles keep the last item while the dialog animates closed.
  const confirmShown = useLast(confirm)
  const dupShown = useLast(dup)
  // Bumped when the duplicate dialog closes, so a request that finishes afterwards doesn't navigate.
  const dupRequest = useRef(0)
  const closeDup = () => {
    dupRequest.current++
    setDup(null)
  }
  const lock = useConfigLock()
  const list = acts.data?.activities
  const activeId = status.data ? status.data.active_activity_id : (acts.data?.active_activity_id ?? null)
  const blocked = lock.state === 'blocked'

  const run = async (id: string | 'off') => {
    setPending(id)
    await switchActivity(id, (x) => list?.find((a) => a.id === x)?.name ?? x)
    setPending(null)
  }

  const askDuplicate = (a: Activity) => setDup({ from: a, name: `${a.name} copy`.slice(0, LIMITS.nameMax), busy: false })
  const duplicate = async (e: FormEvent) => {
    e.preventDefault()
    if (!dup || !dup.name.trim()) return
    const { from: a, name } = dup
    const request = ++dupRequest.current
    setDup({ ...dup, busy: true })
    try {
      const copy = await api.createActivity({ name: name.trim(), startup: a.startup, shutdown: a.shutdown, bindings: a.bindings })
      invalidate('activities')
      toast('Activity duplicated', { variant: 'success' })
      if (request !== dupRequest.current) return // closed meanwhile: the copy is in the list, stay here
      closeDup()
      navigate(`/activities/${copy.id}`)
    } catch (err) {
      setDup((d) => d && { ...d, busy: false })
      toastError('Couldn’t duplicate', err)
    }
  }

  const remove = async (a: Activity) => {
    try {
      await api.deleteActivity(a.id, a.revision)
      invalidate('activities')
      toast(`Deleted ${a.name}`, { variant: 'success' })
    } catch (e) {
      toastError('Couldn’t delete', e)
    }
    setConfirm(null)
  }

  return (
    <>
      <PageHeader
        title="Activities"
        actions={
          list?.length ? (
            <Button variant={activeId ? 'default' : 'outline'} onClick={() => run('off')} disabled={!activeId || !!pending || blocked} title={lock.title}>
              {pending === 'off' ? <Loader2 className="animate-spin" /> : <Power />}
              Off
            </Button>
          ) : undefined
        }
      />
      {acts.error ? (
        <ErrorState error={acts.error} retry={acts.refetch} />
      ) : !list ? (
        <div className={GRID}>
          {[0, 1, 2].map((i) => <Skeleton key={i} className="h-14" />)}
        </div>
      ) : list.length === 0 ? (
        <EmptyState
          icon={ActivityIcon}
          title="No activities yet"
          action={
            <div className="flex flex-wrap justify-center gap-2">
              <Link href="/activities/new" className={buttonVariants()}>
                New activity
              </Link>
              <GlobalLink />
            </div>
          }
        >
          An activity turns on your devices and sets up the remote for them.
        </EmptyState>
      ) : (
        <ul className={GRID}>
          {list.map((a) => {
            const on = a.id === activeId
            return (
              <li key={a.id} className="relative">
                {/* The running tile does nothing on tap: re-running its startup could toggle devices off. */}
                <button
                  onClick={on ? undefined : () => run(a.id)}
                  disabled={!!pending || blocked}
                  aria-disabled={on || undefined}
                  title={lock.title}
                  aria-pressed={on}
                  className={cn(
                    'flex min-h-14 w-full items-center gap-3 rounded-xl border py-3 pr-12 pl-4 text-left shadow-xs transition-colors outline-none focus-visible:ring-[3px] focus-visible:ring-ring/50 disabled:opacity-60',
                    on ? 'border-primary bg-primary text-primary-foreground cursor-default' : 'bg-card hover:bg-accent cursor-pointer',
                  )}
                >
                  <span className="min-w-0 flex-1 truncate font-semibold">{a.name}</span>
                  {pending === a.id ? (
                    <span className="flex shrink-0 items-center gap-1.5 text-xs opacity-80">
                      <Loader2 className="size-3 animate-spin" /> Starting…
                    </span>
                  ) : (
                    on && <span className="shrink-0 rounded-full bg-primary-foreground/20 px-2 py-0.5 text-xs font-medium">On</span>
                  )}
                </button>
                <div className="absolute top-1/2 right-2 -translate-y-1/2">
                  <Button variant="ghost" size="icon" className={cn('size-8', on ? 'text-primary-foreground hover:bg-primary-foreground/15 hover:text-primary-foreground' : 'text-muted-foreground')} aria-label={`More for ${a.name}`} aria-expanded={menu === a.id} onClick={() => setMenu(menu === a.id ? null : a.id)}>
                    <MoreHorizontal />
                  </Button>
                  {menu === a.id && (
                    <div role="menu" aria-label={a.name} className="absolute top-full right-0 z-20 mt-1 grid w-40 rounded-md border bg-popover p-1 shadow-lg" onKeyDown={(e) => menuKeys(e, () => setMenu(null))}>
                      <MenuItem autoFocus icon={Pencil} onClick={() => (setMenu(null), navigate(`/activities/${a.id}`))}>
                        Edit
                      </MenuItem>
                      <MenuItem icon={Copy} disabled={lock.locked} onClick={() => (setMenu(null), askDuplicate(a))}>
                        Duplicate
                      </MenuItem>
                      <MenuItem icon={Trash2} destructive disabled={lock.locked} onClick={() => (setMenu(null), setConfirm(a))}>
                        Delete
                      </MenuItem>
                    </div>
                  )}
                </div>
              </li>
            )
          })}
        </ul>
      )}
      {list && list.length > 0 && (
        <div className="mt-6 flex flex-wrap gap-2">
          {lock.locked ? (
            <Button variant="outline" disabled title={lock.title}>
              <Plus /> New activity
            </Button>
          ) : (
            <Link href="/activities/new" className={buttonVariants({ variant: 'outline' })}>
              <Plus /> New activity
            </Link>
          )}
          <GlobalLink />
        </div>
      )}
      <Dialog
        open={!!dup}
        onClose={closeDup}
        title={`Duplicate ${dupShown?.from.name ?? ''}`}
        footer={
          <>
            <Button variant="ghost" onClick={closeDup}>
              Cancel
            </Button>
            <Button type="submit" form="dup-form" disabled={!dup?.name.trim() || dup?.busy}>
              Duplicate
            </Button>
          </>
        }
      >
        <form id="dup-form" onSubmit={duplicate}>
          <Field label="Name" htmlFor="dup-name">
            <Input id="dup-name" value={dupShown?.name ?? ''} maxLength={LIMITS.nameMax} onChange={(e) => setDup((d) => d && { ...d, name: e.target.value })} autoFocus required />
          </Field>
        </form>
      </Dialog>
      <Dialog
        open={!!confirm}
        onClose={() => setConfirm(null)}
        title={`Delete ${confirmShown?.name ?? ''}?`}
        description={activeId === confirmShown?.id ? 'It’s on now. Hubbo will switch to the next activity.' : 'This can’t be undone.'}
        footer={
          <>
            <Button variant="ghost" onClick={() => setConfirm(null)}>
              Cancel
            </Button>
            <Button variant="destructive" onClick={() => confirm && remove(confirm)}>
              Delete
            </Button>
          </>
        }
      />
    </>
  )
}

function GlobalLink() {
  return (
    <Link href="/activities/global" className={buttonVariants({ variant: 'ghost' })}>
      <Globe /> Global buttons
    </Link>
  )
}

function MenuItem({ icon: Icon, destructive, children, ...props }: { icon: LucideIcon; destructive?: boolean } & ComponentProps<'button'>) {
  return (
    <button
      role="menuitem"
      className={cn(
        'flex items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm outline-none hover:bg-accent focus:bg-accent disabled:opacity-50 disabled:pointer-events-none',
        destructive && 'text-destructive hover:bg-destructive/10 focus:bg-destructive/10',
      )}
      {...props}
    >
      <Icon className="size-4" /> {children}
    </button>
  )
}

/** `v`, or the last non-null `v` while it's null (e.g. a closing dialog's subject). */
function useLast<T>(v: T | null): T | null {
  const [last, setLast] = useState(v)
  if (v !== null && v !== last) setLast(v)
  return v ?? last
}

/** Menu keys: arrows, Home and End move between items; Escape and Tab close it (focus goes back to its button). */
function menuKeys(e: KeyboardEvent<HTMLElement>, close: () => void) {
  const items = [...e.currentTarget.querySelectorAll<HTMLElement>('[role="menuitem"]:not(:disabled)')]
  const i = items.indexOf(document.activeElement as HTMLElement)
  const go = (j: number) => (e.preventDefault(), items[(j + items.length) % items.length]?.focus())
  if (e.key === 'ArrowDown') go(i + 1)
  else if (e.key === 'ArrowUp') go(i - 1)
  else if (e.key === 'Home') go(0)
  else if (e.key === 'End') go(items.length - 1)
  else if (e.key === 'Tab') close()
  else if (e.key === 'Escape') {
    e.preventDefault()
    const trigger = e.currentTarget.previousElementSibling as HTMLElement | null
    close()
    trigger?.focus()
  }
}
