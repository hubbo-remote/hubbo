# Activity runs: keep going, and show what happened

A sequence that stops at its first failure leaves a room half switched, and if nothing shows it,
nobody knows why the receiver is on the wrong input. So a failed step never stops a sequence, and
the hub keeps the last few runs, step by step, for the web admin to show.

## Keep going

- Applies to every sequence: startup, shutdown, button bindings, smart buttons, macros, API runs.
  One rule.
- A failed or timed-out step is recorded with its error, and the next step starts at once. A `wait`
  still waits. There are no retries.
- **Cancelled:** switching activity (or Off) mid-run cancels the rest of the old run. The steps
  that didn't run are recorded as cancelled.
- **Outcome** of a run: `completed` (every step ok), `completed_with_errors` (it reached the end, and
  one or more steps failed), `cancelled`, `rejected` (couldn't be queued).

## The run history

The hub keeps the **last 8 runs** in RAM (lost on reboot, nothing written to flash): startups,
shutdowns (Off) and macros (see [macros.md](macros.md)). Single-command button presses aren't kept;
they're too frequent to be useful here.

A run:

| Field | Meaning |
|---|---|
| `activity_id` | the activity |
| `kind` | `startup`, `shutdown` or `macro` |
| `started_ago_ms` | how long ago it started |
| `duration_ms` | start to the last step's result, or null while running |
| `outcome` | as above, or `running` |
| `steps` | one per action, in order |

A step: `index`, `device_id` (null for system steps), `command_id`, `result` (`ok`, `failed`,
`timed_out`, `cancelled`, `skipped` and `no_reply` from [state-checks.md](state-checks.md), or
`pending` while running), `error` (the error code, or null), `duration_ms` (null if it didn't run).

- `GET /activity-runs` returns them newest first. Readable with an API token, like `/status`.
- `/status` has `last_run`: the newest run's `activity_id`, `kind`, `outcome` and failed step count,
  so the web admin can flag a problem without fetching the history.
- The history is fixed size, allocates nothing on the RF path, and is read from the API's task
  through a snapshot, as the engine's other state is.

## Web admin

- **Status → Activity Log:** the last runs, newest first. Each run is a line (time, activity,
  start or stop, outcome) with its steps under it: the device, the command, and the result in plain
  words ("timed out after 5 s", "already on") or how long it took.
- **Activity page:** a Log tab with the same log for that activity only. If the newest run of this
  activity had failures, the failing steps in its startup or shutdown list carry a small warning with
  the reason.
- **Overview:** when the newest run had failures, one line names the activity and links to its Log tab.

## Not yet

A "wait until on" step to replace fixed waits.
