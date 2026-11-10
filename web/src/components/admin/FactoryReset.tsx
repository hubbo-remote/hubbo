import { Eraser, Loader2 } from 'lucide-react'
import { useEffect, useState } from 'react'
import { api, errorMessage } from '@/api/client'
import { Section, TypedWordField } from '@/components/common'
import { Alert } from '@/components/ui/alert'
import { Button } from '@/components/ui/button'
import { Dialog } from '@/components/ui/dialog'
import { sleep, typedMatches } from '@/lib/utils'

/** The word typed to confirm a factory reset (not "reset": that's the configuration reset). */
const ERASE_WORD = 'erase'
const POLL_MS = 1000

/**
 * Erases everything (POST /admin/factory-reset), then waits for the hub to come back fresh and
 * reloads into the setup wizard. Polls the public session, not /status: every session is gone, and
 * a 401 elsewhere already sends the app to its session check (which lands on setup too).
 */
export function FactoryReset() {
  const [confirm, setConfirm] = useState(false)
  const [typed, setTyped] = useState('')
  const [state, setState] = useState<'idle' | 'sending' | 'erasing'>('idle')
  const [error, setError] = useState<unknown>(null)
  // Erasing: wait for the hub to come back fresh, then reload into setup. Stops on unmount.
  useEffect(() => {
    if (state !== 'erasing') return
    let live = true
    void (async () => {
      while (live) {
        await sleep(POLL_MS)
        const s = await api.session().catch(() => null) // not answering yet: still restarting
        if (live && s?.auth_state === 'fresh') {
          window.location.hash = '#/'
          return window.location.reload()
        }
      }
    })()
    return () => void (live = false)
  }, [state])

  const close = () => {
    if (state !== 'idle') return
    setConfirm(false)
    setTyped('')
    setError(null)
  }

  const erase = async () => {
    setState('sending')
    setError(null)
    try {
      await api.factoryReset()
    } catch (e) {
      setState('idle')
      return setError(e)
    }
    setState('erasing')
  }

  return (
    <Section
      title="Factory reset"
      description="Erases everything on the hub: the login, API tokens, the Home Assistant token, Bluetooth pairings, the remote and the configuration. The hub restarts into first-run setup. Use it before handing the hub to someone else."
    >
      <Button variant="outline" className="text-destructive hover:text-destructive" onClick={() => setConfirm(true)}>
        <Eraser /> Factory reset…
      </Button>
      <Dialog
        open={confirm}
        onClose={close}
        title="Erase everything on the hub?"
        description="This can’t be undone. Download a backup first if you might want this configuration again."
        footer={
          state === 'erasing' ? undefined : (
            <>
              <Button variant="ghost" onClick={close} disabled={state !== 'idle'}>
                Cancel
              </Button>
              <Button variant="destructive" onClick={erase} disabled={state !== 'idle' || !typedMatches(typed, ERASE_WORD)}>
                {state === 'sending' && <Loader2 className="animate-spin" />} Erase everything
              </Button>
            </>
          )
        }
      >
        {state === 'erasing' ? (
          <p className="inline-flex items-center gap-2 text-sm" role="status">
            <Loader2 className="size-4 animate-spin text-primary" /> Erasing… the hub is restarting. Setup opens when it’s back.
          </p>
        ) : (
          <div className="grid gap-4">
            <p className="text-sm text-muted-foreground">Afterwards, also forget “Hubbo” in the Apple TV’s Bluetooth settings, so it can pair again.</p>
            {error != null && (
              <Alert variant="error" title="Nothing was erased">
                <p>{errorMessage(error)}</p>
              </Alert>
            )}
            <TypedWordField word={ERASE_WORD} id="erase-confirm" value={typed} onChange={setTyped} />
          </div>
        )}
      </Dialog>
    </Section>
  )
}
