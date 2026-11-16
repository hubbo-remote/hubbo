import { Activity as ActivityIcon, Copy, Loader2, MoreHorizontal, Pencil, Plus, Power, Trash2 } from 'lucide-react'
import { useRef, useState, type FormEvent } from 'react'
import { Link, useLocation } from 'wouter'
import { api } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Activity } from '@/api/types'
import { EmptyState, ErrorState, PageHeader } from '@/components/common'
import { Button, buttonVariants } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { Field, Input } from '@/components/ui/input'
import { Menu, MenuItem, MenuSeparator } from '@/components/ui/menu'
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
      const copy = await api.createActivity({ name: name.trim(), startup: a.startup, shutdown: a.shutdown, bindings: a.bindings, macros: a.macros })
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
            <Link href="/activities/new" className={buttonVariants()}>
              New activity
            </Link>
          }
        >
          An activity turns on your devices and sets up the remote for them.
        </EmptyState>
      ) : (
        <ul className={GRID} aria-label="Activities">
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
                    on ? 'border-primary bg-primary text-primary-foreground' : 'bg-card hover:bg-accent',
                  )}
                >
                  <span className="min-w-0 flex-1 truncate font-semibold">{a.name}</span>
                  {/* aria-pressed says it's on; the highlight shows it. */}
                  {pending === a.id && (
                    <span className="flex shrink-0 items-center gap-1.5 text-xs opacity-80">
                      <Loader2 className="size-3 animate-spin" /> Starting…
                    </span>
                  )}
                </button>
                <Menu
                  className="absolute top-1/2 right-2 -translate-y-1/2"
                  label={a.name}
                  open={menu === a.id}
                  onOpenChange={(o) => setMenu(o ? a.id : null)}
                  trigger={(p) => (
                    <Button {...p} variant="ghost" size="icon" className={cn('size-8', on ? 'text-primary-foreground hover:bg-primary-foreground/15 hover:text-primary-foreground' : 'text-muted-foreground')} aria-label={`More for ${a.name}`}>
                      <MoreHorizontal />
                    </Button>
                  )}
                >
                  <MenuItem icon={Pencil} onClick={() => navigate(`/activities/${a.id}`)}>
                    Edit
                  </MenuItem>
                  <MenuItem icon={Copy} disabled={lock.locked} onClick={() => askDuplicate(a)}>
                    Duplicate
                  </MenuItem>
                  <MenuSeparator />
                  <MenuItem icon={Trash2} destructive disabled={lock.locked} onClick={() => setConfirm(a)}>
                    Delete
                  </MenuItem>
                </Menu>
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

/** `v`, or the last non-null `v` while it's null (e.g. a closing dialog's subject). */
function useLast<T>(v: T | null): T | null {
  const [last, setLast] = useState(v)
  if (v !== null && v !== last) setLast(v)
  return v ?? last
}

