# Remote log: what happened to the last few minutes of presses

Approved by the user on 2026-11-23. When a press seems to go missing, the user says so right away
and the lead reads the log. It answers one question: did the press reach the hub, and if it did,
what happened to it?

## Dev builds only (user, 2026-11-23)

The log is compiled into dev builds only: any build not made by the release workflow
(`HUBBO_FW_VERSION` unset), which includes the dev board and test builds handed to the user. A
release build has no log, no memory for it, and `GET /remote/log` answers 404. hub-sim always has
it.

## What it keeps

A rolling log of the last **5 minutes**, in RAM only (lost on restart), always on. It's bounded
by count too (a full 5 minutes of held buttons can't grow it without limit; the oldest entries go
first). Each entry has the hub's uptime in ms and one of:

- **packet:** a payload the radio received. Button, kind (down, hold or repeat, up), and the
  signal flag: `strong`, `weak` or `unknown` (a radio without the detector), from the nRF24's
  received power detector. Payloads queued together share the newest packet's flag.
- **event:** what the remote input turned it into: tap, hold, release, and a release the hub had
  to time out because the up packet never came (`timed_out`).
- **action:** what the press did. The binding (command, macro or activity), the target (Bluetooth
  or Home Assistant), the result (ok or the error), and how long it took.
- **anomaly:** a bad checksum, an unknown code, or the RF loop stalling past 5 ms around that
  time.

The writers are the RF loop and the tasks that carry out actions. Writing must never block the RF
loop: a short spin-locked append into a preallocated ring (PSRAM is fine), nothing that can wait.

## Reading it

- `GET /remote/log`: `{span_ms, dropped, entries}`, entries oldest first, each with `type` and
  `age_ms` (the hub has no wall clock). `span_ms` is how far back the log is complete; `dropped`
  counts entries pushed out by the count bound.
- **Readable with an API token** (like `/remote/status`), so the lead can read a hub in daily use
  without admin access.
- There's no web admin page for now. The user tells the lead, and the lead reads it.

## Owners

- **core:** the log (types, ring, 5-minute and count bounds), the hooks in remote input, the
  engine's action results (HA and Bluetooth results arrive through the engine's completion, once
  per step); hub-sim fills it from simulated presses.
- **api:** the route, its role (API tokens may read it), spec, fixtures, contract,
  docs/agent-api.md.
- **firmware:** the signal flag per packet, the RF loop stall anomaly, the Bluetooth send
  results, and checking that the loop gap is unchanged on .196.
