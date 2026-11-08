/** Hubbo mark: a rounded remote silhouette with a ring and a dot. Original artwork. */
export function Logo({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 32 32" className={className} aria-hidden>
      <rect x="9" y="2" width="14" height="28" rx="7" className="fill-primary" />
      <circle cx="16" cy="12" r="4" fill="none" className="stroke-primary-foreground" strokeWidth="2" />
      <circle cx="16" cy="22" r="1.6" className="fill-primary-foreground" />
    </svg>
  )
}
