import { Activity, Gauge, Grid3x3, LogOut, Monitor, Moon, Plug, Settings, Sun, Tv2, type LucideIcon } from 'lucide-react'
import { useState, type ReactNode } from 'react'
import { Link, useLocation } from 'wouter'
import { api } from '@/api/client'
import type { Session } from '@/api/types'
import { Button } from '@/components/ui/button'
import { Menu, MenuItem, MenuSeparator } from '@/components/ui/menu'
import { useActivities, useStatus } from '@/hooks/data'
import { useTheme, type ThemePref } from '@/lib/theme'
import { cn } from '@/lib/utils'
import { ConfigBanner } from './ConfigBanner'
import { Logo } from './Logo'

/**
 * `prefix`: where the section's other pages live, when not under `href` (home's editors are under /activities).
 * `desktopOnly`: left out of the phone tab bar (Settings is in the phone header's gear menu).
 */
const NAV: { href: string; prefix?: string; label: string; icon: LucideIcon; desktopOnly?: boolean }[] = [
  { href: '/', prefix: '/activities', label: 'Activities', icon: Activity },
  { href: '/devices', label: 'Devices', icon: Tv2 },
  { href: '/remote', label: 'Remote', icon: Grid3x3 },
  { href: '/integrations', label: 'Integrations', icon: Plug },
  { href: '/status', label: 'Status', icon: Gauge },
  { href: '/settings', label: 'Settings', icon: Settings, desktopOnly: true },
]

function useActive() {
  const [loc] = useLocation()
  return (n: (typeof NAV)[number]) => loc === n.href || loc.startsWith((n.prefix ?? n.href) + '/')
}

const THEMES: { id: ThemePref; icon: LucideIcon; label: string }[] = [
  { id: 'light', icon: Sun, label: 'Light' },
  { id: 'dark', icon: Moon, label: 'Dark' },
  { id: 'system', icon: Monitor, label: 'System' },
]

export function ThemeToggle({ className }: { className?: string }) {
  const [theme, setTheme] = useTheme()
  return (
    <div role="radiogroup" aria-label="Theme" className={cn('inline-flex rounded-lg bg-muted p-0.5', className)}>
      {THEMES.map((t) => (
        <button
          key={t.id}
          role="radio"
          aria-checked={theme === t.id}
          aria-label={t.label}
          title={t.label}
          onClick={() => setTheme(t.id)}
          className={cn('grid size-7 place-items-center rounded-md text-muted-foreground transition-colors outline-none focus-visible:ring-2 focus-visible:ring-ring', theme === t.id && 'bg-card text-foreground shadow-sm')}
        >
          <t.icon className="size-3.5" />
        </button>
      ))}
    </div>
  )
}

/** The phone header's gear: the theme (Light, Dark, System) and the way to Settings, so the tab bar stays at five. */
function SettingsMenu({ className }: { className?: string }) {
  const [open, setOpen] = useState(false)
  const [theme, setTheme] = useTheme()
  const [, navigate] = useLocation()
  return (
    <Menu
      className={className}
      label="Settings"
      open={open}
      onOpenChange={setOpen}
      trigger={(p) => (
        <Button {...p} variant="ghost" size="icon" aria-label="Settings and theme">
          <Settings />
        </Button>
      )}
    >
      {THEMES.map((t) => (
        <MenuItem key={t.id} icon={t.icon} checked={theme === t.id} onClick={() => setTheme(t.id)}>
          {t.label}
        </MenuItem>
      ))}
      <MenuSeparator />
      <MenuItem icon={Settings} onClick={() => navigate('/settings')}>
        Settings
      </MenuItem>
    </Menu>
  )
}

/** What's on, quietly, under the brand: the running activity's name, or Off. Status only, no actions. */
function NowOn() {
  const status = useStatus()
  const acts = useActivities()
  if (!status.data) return null
  const id = status.data.active_activity_id
  const name = id ? (acts.data?.activities.find((a) => a.id === id)?.name ?? id) : null
  return (
    <span role="status" className="flex min-w-0 items-center gap-1.5 text-xs font-normal text-muted-foreground">
      <span className={cn('size-1.5 shrink-0 rounded-full', name ? 'bg-success' : 'bg-muted-foreground/40')} aria-hidden />
      <span className="truncate">{name ?? 'Off'}</span>
    </span>
  )
}

/** The logo, "Hubbo", and what's on under it. */
function Brand({ small }: { small?: boolean }) {
  return (
    <div className="flex min-w-0 items-center gap-2">
      <Logo className={small ? 'size-6 shrink-0' : 'size-7 shrink-0'} />
      <div className="grid min-w-0 leading-tight">
        <span className={cn('font-semibold', !small && 'text-lg tracking-tight')}>Hubbo</span>
        <NowOn />
      </div>
    </div>
  )
}

export function AppShell({ session, onSignedOut, children }: { session: Session; onSignedOut: () => void; children: ReactNode }) {
  const isActive = useActive()
  const status = useStatus()
  const sim = status.data?.auth_mode === 'simulator_no_auth' || session.auth_mode === 'simulator_no_auth'

  const logout = async () => {
    await api.logout().catch(() => undefined)
    onSignedOut()
  }

  return (
    <div className="min-h-dvh md:grid md:grid-cols-[15rem_minmax(0,1fr)]">
      {/* Hash routing: "#main" would be a route, so move focus instead of following the link. */}
      <a
        href="#main"
        onClick={(e) => {
          e.preventDefault()
          document.getElementById('main')?.focus()
        }}
        className="sr-only focus:not-sr-only focus:fixed focus:top-2 focus:left-2 focus:z-50 focus:rounded-md focus:bg-card focus:px-3 focus:py-2 focus:shadow">
        Skip to content
      </a>
      {/* Desktop sidebar */}
      <div className="hidden border-r bg-card/50 md:block">
      <aside className="sticky top-0 flex h-dvh flex-col">
        <div className="flex h-16 items-center px-5">
          <Brand />
        </div>
        <nav aria-label="Main" className="grid gap-0.5 px-3">
          {NAV.map((n) => (
            <Link
              key={n.href}
              href={n.href}
              aria-current={isActive(n) ? 'page' : undefined}
              className={cn(
                'flex items-center gap-3 rounded-md px-3 py-2 text-sm font-medium text-muted-foreground transition-colors outline-none hover:bg-accent hover:text-accent-foreground focus-visible:ring-2 focus-visible:ring-ring',
                isActive(n) && 'bg-accent text-accent-foreground',
              )}
            >
              <n.icon className="size-4" aria-hidden />
              {n.label}
            </Link>
          ))}
        </nav>
        <div className="mt-auto grid gap-3 border-t p-4">
          <div className="flex items-center justify-between">
            <ThemeToggle />
            <Button variant="ghost" size="icon" onClick={logout} aria-label="Sign out" title="Sign out">
              <LogOut />
            </Button>
          </div>
          <p className="truncate text-xs text-muted-foreground">
            Signed in as <span className="font-medium text-foreground">{session.username}</span>
          </p>
        </div>
      </aside>
      </div>

      <div className="flex min-w-0 flex-col">
        {sim && (
          <div role="alert" className="bg-warning px-4 py-1.5 text-center text-xs font-bold tracking-wide text-black">
            AUTH DISABLED — SIMULATOR
          </div>
        )}
        <ConfigBanner admin={session.role === 'admin'} />
        {/* Mobile header */}
        <header className="sticky top-0 z-30 flex h-14 items-center gap-3 border-b bg-background/85 px-4 backdrop-blur md:hidden">
          <Brand small />
          <SettingsMenu className="ml-auto shrink-0" />
        </header>
        <main id="main" tabIndex={-1} className="outline-none mx-auto w-full max-w-6xl flex-1 px-4 pt-4 pb-28 sm:px-6 md:px-8 md:pt-8 md:pb-12">
          {children}
        </main>
      </div>

      {/* Mobile tab bar */}
      <nav aria-label="Main" className="fixed inset-x-0 bottom-0 z-30 grid grid-cols-5 border-t bg-background/95 pb-[env(safe-area-inset-bottom)] backdrop-blur md:hidden">
        {NAV.filter((n) => !n.desktopOnly).map((n) => (
          <Link
            key={n.href}
            href={n.href}
            aria-current={isActive(n) ? 'page' : undefined}
            className={cn('flex flex-col items-center gap-0.5 py-2 text-[11px] font-medium text-muted-foreground outline-none focus-visible:bg-accent', isActive(n) && 'text-primary')}
          >
            <n.icon className="size-5" aria-hidden />
            {n.label}
          </Link>
        ))}
      </nav>
    </div>
  )
}
