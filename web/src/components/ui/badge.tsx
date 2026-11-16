import { cva, type VariantProps } from 'class-variance-authority'
import type { ComponentProps } from 'react'
import { cn } from '@/lib/utils'

const badgeVariants = cva('inline-flex items-center gap-1 rounded-md border px-2 py-0.5 text-xs font-medium whitespace-nowrap [&_svg]:size-3', {
  variants: {
    variant: {
      default: 'border-transparent bg-primary text-primary-foreground',
      secondary: 'border-transparent bg-secondary text-secondary-foreground',
      outline: 'text-foreground',
      // On their own tint the base colours fall short of 4.5:1 in light mode, so the text is darkened there (as warning's).
      success: 'border-transparent bg-success/15 text-[color-mix(in_oklch,var(--success),black_20%)] dark:text-success',
      warning: 'border-transparent bg-warning/20 text-[color-mix(in_oklch,var(--warning),black_35%)] dark:text-warning',
      destructive: 'border-transparent bg-destructive/15 text-[color-mix(in_oklch,var(--destructive),black_30%)] dark:text-destructive',
    },
  },
  defaultVariants: { variant: 'default' },
})

export function Badge({ className, variant, ...props }: ComponentProps<'span'> & VariantProps<typeof badgeVariants>) {
  return <span className={cn(badgeVariants({ variant }), className)} {...props} />
}
