# Persistence and API contracts

The rules for saved state and for what the API reports. They exist because the obvious approach
(write a file, rename it, trust what loads) produced a string of bugs: zero revisions, dropped
fields, defaults written over a config that was only unreadable. Each rule below closes a class of
those.

## One owner for saved state; the hub runs exactly what's saved

**Record store (core, `storage/`).** Each saved record (`config`, `active`, `secrets`) is two slot
files, `<name>.a` and `<name>.b`. Every slot starts with a header: magic, format version,
generation (u64), payload length and CRC32. The payload follows.

- **Save** writes the slot that doesn't hold the newest intact record, reads it back to verify,
  and then the save is acknowledged. There are no renames and no temp files. If verification
  fails, that slot is invalidated (its header zeroed) before an error is returned. So an
  unacknowledged save can never load later.
- **Load** returns the intact record with the highest generation. "Intact" means the header and
  CRC check out, and nothing else. The store never judges content.
- **Health** is explicit: `Available`, `Unavailable` (mount or IO failure) or `ReadOnly`. When the
  store isn't available, a save fails with `StorageFailed`. Nothing falls back to RAM.
- Every platform implements the store over its own medium: LittleFS files on the board, files
  under `--data` in hub-sim, memory in tests. All of them pass the same crash-injection tests.
- On the board, secrets live in NVS, behind the same interface and contract: atomic per record,
  and health reported. NVS writes each entry atomically, and login still works if LittleFS
  breaks, so the hub stays reachable for a restore. Hub-sim uses files for all three records.

**"Can't read" is never "doesn't exist".** A read error or unavailable medium is reported as
unknown, never as "absent", and nothing that depends on absence may act on it: no defaults saved
over an unreadable config, no first-run admin setup over unreadable secrets (that would be an
account takeover), no whole-record save from a store whose contents weren't read. Stores report
`NotFound` only when the medium is healthy and positively says so (NVS: `ESP_ERR_NVS_NOT_FOUND`),
and owners re-read from storage before any decision that rests on "absent". Credentials have the
same explicit states as the config (`Fresh`, `Ok`, `Unavailable`, `Corrupt`); only `Fresh` allows
setup.

**ConfigService states.**
- `Fresh`: no intact config record. Start from the defaults, which already have revisions set.
- `Ok`.
- `Blocked{reason, generation}`: an intact record exists but can't be used: a newer schema, or a
  compile failure. The hub boots with no active activity, refuses every edit, and keeps both slots
  untouched. Only an explicit restore or factory reset may write.
- `StorageUnavailable`.

Status and the API expose the state, and the web admin shows it.

**The hub runs what's saved.** A commit goes: build the candidate → serialize → parse the bytes
back → compile the parsed config → save → publish the parsed and compiled result. Nothing that
can't round-trip can ever run, so a field that isn't saved can't be live.

**Schema versions.** The config carries `schema_version`. A newer one is Blocked. An older one
would be migrated on the parsed record before compiling and saved once as a new generation, but
nothing has been released that needs it, so there are no migrations and no compatibility shims.

**Shutdown flush.** Pending saves (the debounced active activity) are written out on a graceful
stop: hub-sim SIGTERM, a firmware update's reboot, or a restart. A power cut loses at most the
debounce window.

## One generation counter

- The config has a single saved, monotonic `generation`, starting at 1: the record generation
  above.
- Every committed change bumps it.
- Each object's `revision` is the generation of its last change. The same goes for global
  bindings, smart buttons and integrations. There are no separate counters and no zeros.
- Restore and reset stamp every object with the new generation, so stale clients always get 409.
  A backup that omits revisions or has revision 0 is accepted; the stamping replaces them.

## Integrations: one source of truth

- `config.integrations` is authoritative and always complete. The built-ins (`system`, `ble`, `ir`,
  `ha_main`, `mqtt_main`) are always present, put there when the config is loaded or created.
- One resolver, used by both `compileConfig` and `compileDeviceCommand`, maps device →
  integration → adapter. It checks that the integration exists, that the provider type matches,
  that the integration is `enabled`, and that an adapter is registered.
- A disabled integration's devices compile to "unavailable" (`integration_disabled`). Execute
  and bindings report that rather than reaching the adapter.

## API responses tell the truth

- **Command effects.** Each command in the catalog declares an `effect`, either `momentary` or
  `hold` (a hold puts a state that must be released, such as `key_down`). Capabilities include
  it. Direct execute accepts only `momentary` commands and answers 422 for `hold`. Bindings may
  use `hold`, and the validator checks every hold has its release (the down/up pairing).
- **Transitions** report what happened in each phase: `switched` (bool), the new and previous
  active activity, and `startup: queued | rejected{reason}` (shutdown likewise). A transition
  that switched is never reported as a retryable 503.
- **Body limits** have one source: the router's policy table. Transports ask the router for the
  matched route's limit (`bodyLimit(method, path)`) before reading the body, and read it into a
  per-request buffer (PSRAM on the board). `auth::maxRequestBody()` sets the absolute cap.

## Further rules

- **Never destroy on uncertain information.** Automatic destructive actions (formatting, writing
  defaults, clearing state) happen only on positive knowledge. LittleFS is formatted automatically
  only when the partition reads as erased flash (all `0xFF`); otherwise a mount failure means
  `StorageUnavailable` until the admin explicitly asks for a format. There's no "already
  formatted" flag: it would be a second, unreliable source of truth.
- **A destructive action needs its trigger cleared first.** A requested format runs only after the
  request flag has been durably cleared; otherwise the hub doesn't format.
- **Absence needs proof.** A record is absent only when no slot file exists, or every slot that
  exists is one we deliberately invalidated (zeroed header). If a slot is present but fails its
  CRC, uses an unknown format or is too large, and no slot is intact, the record is **Corrupt**,
  never absent: the config goes Blocked and credentials go Corrupt. One torn slot next to an intact
  one still falls back. Accepted cost: if power fails during the very first save of a record, the
  next boot shows Corrupt and needs a reset.
- **Secondary state follows primary state.** State derived from the config (the active activity)
  is never written while the config isn't `Ok`. A failed restore against a Blocked or unavailable
  config means "unknown", not "none".
- **The record store owns generations.** Owners never cache the next generation; they ask the
  store. If the store can't invalidate a slot after a failed save, it reports itself `ReadOnly`, so
  no later save or acknowledgement happens on a medium it can't trust. Trust and health belong to
  the medium, so every record store on it sees the same state.
- **Don't present a guess as data.** While the config is Blocked, a backup exports the saved
  record's raw bytes, clearly marked as blocked, rather than the empty runtime config.
- **Restore and reset never start an activity.** After a restore or reset, the active activity
  is kept if its id still exists; otherwise the hub becomes inactive without running any startup or
  shutdown. The "switch to the next activity" rule applies only to deleting the active activity.
- **Held keys are guaranteed at runtime, not by validation alone.** The engine keeps a ledger of
  holds per button: which adapter, and which release command. When that button goes up (from any
  binding, global or activity), its sequence fails or is cancelled, the radio times out, or
  there's a transition, every hold still open for it is released. A hold is forgotten only once its
  release has completed; a release that fails falls back to `releaseAll`. Static validation (holds
  only on `:down`, a matching release) is early feedback, not the guarantee.
- **One pipeline per operation.** A dry run is the real commit pipeline without the final save:
  the same normalization, built-ins, stamping, size limit and round trip. Every config edit runs
  the same steps in the same order: editable check, revision check, build, round trip, save,
  publish.
- **One boot path.** "Load config → load credentials → restore the active activity" is a single
  core helper that the firmware and hub-sim both call.
- **Never block across tasks.** The RF/engine task never waits for another task and never touches
  flash or NVS. A reboot is a request: httpd does the flush and then restarts. Recovery retries
  (config and credential refresh) back off exponentially instead of running on every request.
