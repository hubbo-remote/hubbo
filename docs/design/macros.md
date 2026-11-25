# Macros: a button runs a list of commands

A button's Press or Long press can be a macro: a list of commands, edited like a startup or shutdown
list and run by the same sequence executor. A binding has always been stored as a list; a macro is
marked as one, so it stays a macro from its first step.

## Rules

- **Command and macro are two different things.** Every macro starts with one command, so a macro
  is marked, never inferred from its length. An empty Press or Long press offers Command or Macro.
  There's no conversion: to change one into the other, clear it and choose again.
- **No press types inside a macro.** A macro is a plain list, like startup and shutdown: the same
  picker (including a Bluetooth press's timed hold), waits allowed, no hold-while-held commands. A
  single command on Press keeps passing the button straight through.
- **Macros get their own queue**, so a macro with a wait never holds up other buttons.
- **The log shows them** as macros, with the button and what each step did.
- **Advanced triggers** (down, up, tap) stay single commands. A button with a macro can't switch to
  Advanced.
- **Run from the editor:** Run plays the macro as it is on screen, saved or not.

## How it works

- **The mark:** each activity and the global bindings have `macros`, a list of trigger keys next to
  `bindings` (e.g. `["guide:down"]`). Only `<button>:down`, `:tap` or `:hold`, each bound, no repeats,
  at most 144. Left out, it means none. A marked trigger is a macro at any length (1 to 32 actions).
  An unmarked trigger with more than one action is refused (`macro_required`); a listed trigger with
  no actions is `unbound_macro`.
- **Rules in a macro:** waits (including `wait_connected`) are allowed; hold commands and their
  releases are `hold_not_allowed`. A list made only of waits, macro or not, is `sequence_only`.
- **A third executor lane, Macro.** Marked bindings run there, routed by the mark. One runs and at
  most **4** wait; a press when it's full is refused and counted, never blocking, and not recorded.
  It shares the 64-node action pool, but macros together hold at most 32 nodes, so presses always
  have room. No new task, no allocation on the RF path.
- **Activity switches** cancel running and queued macros (the generation guard).
- **No state checks** in macros. The 1 s HA reply limit applies ([state-checks.md](state-checks.md)).
- **Run history** records macro runs alongside startups and shutdowns (8 in all; macros take at
  most 5, and a running run is never replaced): kind `macro`, the `button`, the `trigger` (`press`
  or `long_press`) and the activity (null for a global macro). See
  [activity-runs.md](activity-runs.md).
- **Run from the editor:** `POST /macros/run` with `{actions, trigger, activity_id}` validates the
  list exactly as a saved macro, queues it on the Macro lane and answers `202 {run_id}`; a full lane
  is `503 busy`. The activity needn't be active and isn't switched to.

## In the web admin

- Command or Macro on an empty Press or Long press row. A macro row has the startup editor's list
  (add, add wait, drag, duplicate, remove), Run and Clear. Clearing a macro asks first; clearing a
  single command doesn't.
- The remote picture marks a macro button.
- The Activity Log shows "<Button> · macro" or "<Button> · long press macro" with its steps.
