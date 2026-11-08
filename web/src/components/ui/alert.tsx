import { AlertTriangle, CheckCircle2, Info, XCircle } from 'lucide-react'
import type { ReactNode } from 'react'
import { cn } from '@/lib/utils'

const tone = {
  info: ['border-border bg-muted/50', Info, 'text-muted-foreground'],
  success: ['border-success/30 bg-success/10', CheckCircle2, 'text-success'],
  warning: ['border-warning/40 bg-warning/10', AlertTriangle, 'text-warning'],
  error: ['border-destructive/30 bg-destructive/10', XCircle, 'text-destructive'],
} as const

export function Alert({ variant = 'info', title, children, className, action }: { variant?: keyof typeof tone; title?: ReactNode; children?: ReactNode; className?: string; action?: ReactNode }) {
  const [cls, Icon, iconCls] = tone[variant]
  return (
    <div role={variant === 'error' ? 'alert' : 'status'} className={cn('flex gap-3 rounded-lg border px-4 py-3 text-sm', cls, className)}>
      <Icon className={cn('mt-0.5 size-4 shrink-0', iconCls)} aria-hidden />
      <div className="grid min-w-0 flex-1 gap-1">
        {title && <p className="font-medium leading-tight">{title}</p>}
        {children && <div className="text-muted-foreground [&_p]:leading-relaxed">{children}</div>}
      </div>
      {action}
    </div>
  )
}
