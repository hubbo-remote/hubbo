import { Activity as ActivityIcon, Copy, Globe, MoreHorizontal, Play, Plus, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { Link, useLocation } from 'wouter'
import { api } from '@/api/client'
import { invalidate } from '@/api/hooks'
import type { Activity } from '@/api/types'
import { EmptyState, ErrorState, PageHeader } from '@/components/common'
import { Badge } from '@/components/ui/badge'
import { Button, buttonVariants } from '@/components/ui/button'
import { Card } from '@/components/ui/card'
import { Dialog } from '@/components/ui/dialog'
import { Skeleton } from '@/components/ui/misc'
import { toast, toastError } from '@/components/ui/toast'
import { switchActivity } from '@/hooks/switchActivity'
import { useActivities, useConfigLock, useStatus } from '@/hooks/data'
import { bindingCount, LIMITS } from '@/lib/activity'
import { plural } from '@/lib/utils'

export default function Activities() {
  const acts = useActivities()
  const status = useStatus()
  const [, navigate] = useLocation()
  const [confirm, setConfirm] = useState<Activity | null>(null)
  const [menu, setMenu] = useState<string | null>(null)
  const lock = useConfigLock()

  const duplicate = async (a: Activity) => {
    try {
      const copy = await api.createActivity({ name: `${a.name} copy`.slice(0, LIMITS.nameMax), startup: a.startup, shutdown: a.shutdown, bindings: a.bindings })
      invalidate('activities')
      toast('Activity duplicated', { variant: 'success' })
      navigate(`/activities/${copy.id}`)
    } catch (e) {
      toastError('Could not duplicate', e)
    }
  }

  const remove = async (a: Activity) => {
    try {
      await api.deleteActivity(a.id, a.revision)
      invalidate('activities')
      toast(`Deleted ${a.name}`, { variant: 'success' })
    } catch (e) {
      toastError('Could not delete', e)
    }
    setConfirm(null)
  }

  const start = (a: Activity) => switchActivity(a.id, (x) => acts.data?.activities.find((y) => y.id === x)?.name ?? x)

  return (
    <>
      <PageHeader
        title="Activities"
        description="What the remote does in each mode: startup steps, button bindings and shutdown steps."
        actions={
          <>
            <Link href="/activities/global" className={buttonVariants({ variant: 'outline' })}>
              <Globe /> Global buttons
            </Link>
            {lock.locked ? (
              <Button disabled title={lock.title}>
                <Plus /> New activity
              </Button>
            ) : (
              <Link href="/activities/new" className={buttonVariants()}>
                <Plus /> New activity
              </Link>
            )}
          </>
        }
      />
      {acts.error ? (
        <ErrorState error={acts.error} retry={acts.refetch} />
      ) : !acts.data ? (
        <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-3">
          {[0, 1, 2].map((i) => <Skeleton key={i} className="h-28" />)}
        </div>
      ) : acts.data.activities.length === 0 ? (
        <EmptyState icon={ActivityIcon} title="No activities yet" action={<Link href="/activities/new" className={buttonVariants()}>Create your first activity</Link>}>
          An activity powers on your gear, picks inputs and maps remote buttons. For example: Watch Apple TV.
        </EmptyState>
      ) : (
        <ul className="grid gap-3 sm:grid-cols-2 lg:grid-cols-3">
          {acts.data.activities.map((a) => {
            const running = (status.data?.active_activity_id ?? acts.data!.active_activity_id) === a.id
            const n = bindingCount(a.bindings)
            return (
              <li key={a.id}>
                <Card className="relative flex h-full flex-col transition-shadow focus-within:ring-[3px] focus-within:ring-ring/40 hover:shadow-md">
                  <Link href={`/activities/${a.id}`} className="flex-1 rounded-xl p-5 outline-none after:absolute after:inset-0 after:content-['']">
                    <div className="flex items-start justify-between gap-2">
                      <h2 className="font-semibold">{a.name}</h2>
                      {running && <Badge variant="success">Running</Badge>}
                    </div>
                    <p className="mt-1 text-sm text-muted-foreground">
                      {plural(n, 'button binding')} · {plural(a.startup.length, 'startup step')} · rev {a.revision}
                    </p>
                  </Link>
                  <div className="relative z-10 flex items-center gap-1 border-t px-3 py-2">
                    <Button variant="ghost" size="sm" onClick={() => start(a)} disabled={running || lock.state === 'blocked'} title={lock.state === 'blocked' ? lock.title : undefined}>
                      <Play /> Start
                    </Button>
                    <div className="relative ml-auto">
                      <Button variant="ghost" size="icon" className="size-8" aria-label={`More actions for ${a.name}`} aria-expanded={menu === a.id} onClick={() => setMenu(menu === a.id ? null : a.id)} disabled={lock.locked} title={lock.title}>
                        <MoreHorizontal />
                      </Button>
                      {menu === a.id && (
                        <div role="menu" className="absolute right-0 bottom-full z-20 mb-1 grid w-40 rounded-md border bg-popover p-1 shadow-lg" onKeyDown={(e) => e.key === 'Escape' && setMenu(null)}>
                          <button role="menuitem" autoFocus className="flex items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm hover:bg-accent focus:bg-accent outline-none" onClick={() => (setMenu(null), duplicate(a))}>
                            <Copy className="size-4" /> Duplicate
                          </button>
                          <button role="menuitem" className="flex items-center gap-2 rounded-sm px-2 py-1.5 text-left text-sm text-destructive hover:bg-destructive/10 focus:bg-destructive/10 outline-none" onClick={() => (setMenu(null), setConfirm(a))}>
                            <Trash2 className="size-4" /> Delete
                          </button>
                        </div>
                      )}
                    </div>
                  </div>
                </Card>
              </li>
            )
          })}
        </ul>
      )}
      <Dialog
        open={!!confirm}
        onClose={() => setConfirm(null)}
        title={`Delete ${confirm?.name}?`}
        description={status.data?.active_activity_id === confirm?.id ? 'It’s running now. Hubbo switches to the next activity and runs its startup; this one’s shutdown doesn’t run.' : 'Its bindings and sequences are removed from the hub. This can’t be undone (unless you have a backup).'}
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
