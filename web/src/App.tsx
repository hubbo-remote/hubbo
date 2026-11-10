import { lazy, Suspense, useCallback, useEffect, useState } from 'react'
import { Route, Router, Switch, useLocation } from 'wouter'
import { useHashLocation } from 'wouter/use-hash-location'
import { api, errorMessage, onUnauthorized } from '@/api/client'
import { clearCache } from '@/api/hooks'
import type { Session } from '@/api/types'
import { AppShell } from '@/components/AppShell'
import { Toaster } from '@/components/ui/toast'
import { Spinner } from '@/components/ui/misc'
import { Dashboard } from '@/pages/Dashboard'
import { Login } from '@/pages/Login'
import { NotFound } from '@/pages/NotFound'
import { CredentialsProblem } from '@/pages/CredentialsProblem'

// Code-split: the dashboard ships in the entry chunk, everything else loads on demand.
const Activities = lazy(() => import('@/pages/Activities'))
const ActivityEditor = lazy(() => import('@/pages/ActivityEditor'))
const GlobalBindings = lazy(() => import('@/pages/GlobalBindings'))
const Devices = lazy(() => import('@/pages/Devices'))
const DeviceDetail = lazy(() => import('@/pages/DeviceDetail'))
const Integrations = lazy(() => import('@/pages/Integrations'))
const Settings = lazy(() => import('@/pages/Settings'))
const Setup = lazy(() => import('@/pages/Setup'))

function Loading() {
  return (
    <div className="grid min-h-40 place-items-center text-muted-foreground">
      <Spinner />
    </div>
  )
}

export function App() {
  const [session, setSession] = useState<Session | null>(null)
  const [bootError, setBootError] = useState<string | null>(null)

  const refresh = useCallback(async () => {
    try {
      setSession(await api.session())
      setBootError(null)
    } catch (e) {
      setBootError(errorMessage(e))
    }
  }, [])

  useEffect(() => {
    void refresh()
    return onUnauthorized(() => void refresh())
  }, [refresh])

  const signedOut = useCallback(() => {
    clearCache()
    void refresh()
  }, [refresh])

  let body
  if (!session) {
    body = bootError ? (
      <div className="grid min-h-dvh place-items-center p-6 text-center">
        <div className="grid gap-2">
          <p className="font-medium">Cannot reach the hub</p>
          <p className="text-sm text-muted-foreground">{bootError}</p>
          <button className="text-sm text-primary underline" onClick={() => void refresh()}>
            Try again
          </button>
        </div>
      </div>
    ) : (
      <div className="grid min-h-dvh place-items-center">
        <Spinner />
      </div>
    )
  } else if (!session.authenticated && (session.auth_state === 'unavailable' || session.auth_state === 'corrupt')) {
    // Never the wizard or login: the hub can't read (or trust) its credentials.
    body = <CredentialsProblem kind={session.auth_state} onRetry={refresh} />
  } else if (session.setup_required && session.auth_state === 'fresh') {
    // Only a hub that positively knows it has no admin offers setup (and the
    // no-auth simulator, which is fresh but signed in, never needs it).
    body = (
      <Suspense fallback={<Loading />}>
        <Setup onDone={refresh} />
      </Suspense>
    )
  } else if (!session.authenticated) {
    body = <Login onSignedIn={refresh} />
  } else {
    body = <SignedIn session={session} onSignedOut={signedOut} onSetupDone={refresh} />
  }

  return (
    // Hash routing: the hub's HTTP server only has to serve index.html at "/".
    <Router hook={useHashLocation}>
      {body}
      <Toaster />
    </Router>
  )
}

function SignedIn({ session, onSignedOut, onSetupDone }: { session: Session; onSignedOut: () => void; onSetupDone: () => void }) {
  const [loc] = useLocation()
  // Re-running the wizard is full-screen, like first run.
  const setup = /^\/setup(?:\/([a-z]+))?$/.exec(loc)
  if (setup) {
    return (
      <Suspense fallback={<Loading />}>
        <Setup onDone={onSetupDone} start={setup[1]} />
      </Suspense>
    )
  }
  return (
    <AppShell session={session} onSignedOut={onSignedOut}>
      <Suspense fallback={<Loading />}>
        <Switch>
          <Route path="/" component={Dashboard} />
          <Route path="/activities" component={Activities} />
          <Route path="/activities/global" component={GlobalBindings} />
          <Route path="/activities/new">{() => <ActivityEditor id={null} />}</Route>
          <Route path="/activities/:id">{(p) => <ActivityEditor id={p.id} />}</Route>
          <Route path="/devices" component={Devices} />
          <Route path="/devices/:id">{(p) => <DeviceDetail id={p.id} />}</Route>
          <Route path="/integrations" component={Integrations} />
          <Route path="/settings/:tab?">{(p) => <Settings tab={p.tab ?? 'account'} />}</Route>
          <Route component={NotFound} />
        </Switch>
      </Suspense>
    </AppShell>
  )
}
