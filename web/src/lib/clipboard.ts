/**
 * Copy text, and say whether it worked. The hub serves plain HTTP, where `navigator.clipboard`
 * doesn't exist (it needs a secure context), so this falls back to selecting a hidden textarea and
 * `execCommand('copy')`. `from` is the clicked control: the textarea goes next to it (inside a
 * dialog's focus trap) and focus returns to it afterwards.
 */
export async function copyText(text: string, from?: HTMLElement | null): Promise<boolean> {
  if (window.isSecureContext && navigator.clipboard) {
    try {
      await navigator.clipboard.writeText(text)
      return true
    } catch {
      // Refused (permissions, focus): try the fallback.
    }
  }
  const area = document.createElement('textarea')
  area.value = text
  area.readOnly = true
  area.setAttribute('aria-hidden', 'true')
  area.style.cssText = 'position:fixed;top:0;left:-9999px;opacity:0'
  ;(from?.parentElement ?? document.body).appendChild(area)
  try {
    area.select()
    return document.execCommand('copy')
  } catch {
    return false
  } finally {
    area.remove()
    from?.focus()
  }
}

/** Select an element's text, so it can be copied by hand when copying failed. */
export function selectText(el: HTMLElement | null) {
  if (!el) return
  const range = document.createRange()
  range.selectNodeContents(el)
  const sel = window.getSelection()
  sel?.removeAllRanges()
  sel?.addRange(range)
}
