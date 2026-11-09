# Persistence and API contracts: the Phase 2 redesign

Status: approved 2026-11-08. It replaces piecemeal fixes for the Phase 2 review findings and the
bugs the reboot round-trip tests found (revision 0, the migration flag, dropped fields). Those
were symptoms of four design flaws. This doc fixes the flaws.

## 1. One owner for saved state; the hub runs exactly what's saved

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
  under `--data` in hub-sim, memory in tests. All of them have to pass the same crash-injection
  tests.
- On the board, secrets and the active activity may stay in NVS, as long as they're behind the
  same interface and meet the same contract: atomic per record, and health reported. Hub-sim
  uses files for all three records. Nothing durable uses a memory store outside tests.

**"Can't read" is never "doesn't exist".** This applies to every record (config, active, secrets)
and every store behind them, including NVS. A read error or unavailable medium is reported as
unknown, never as "absent", and nothing that depends on absence may act on it: no defaults saved
over an unreadable config, no first-run admin setup over unreadable secrets (that would be an
account takeover), no whole-record save from a store whose contents weren't read. Stores report
`NotFound` only when the medium is healthy and positively says so (NVS: `ESP_ERR_NVS_NOT_FOUND`),
and owners re-read from storage before any decision that rests on "absent". Credentials have the
same explicit states as the config (`Fresh`, `Ok`, `Unavailable`, `Corrupt`); only `Fresh` allows
setup.

**ConfigService states.**
- `Fresh`: no intact config record. Start from the defaults, which already have revisions set
  (see 2).
- `Ok`.
- `Blocked{reason, generation}`: an intact record exists but can't be used. That covers a newer
  schema, a migration failure or a compile failure. The hub boots with no active activity,
  refuses every edit, and keeps both slots untouched. Only an explicit restore or factory reset
  may write.
- `StorageUnavailable`.

Status and the API expose the state. The UI shows it.

**The hub runs what's saved.** A commit goes: build the candidate → serialize → parse the bytes
back → compile the parsed config → save → publish the parsed and compiled result. Nothing that
can't round-trip can ever run, so a field that isn't saved can't be live.

**Migrations** run on the parsed record before compiling. That includes catalog renames such as
`DIGIT_n` → `KEY_n`. A migrated config is saved once, as a new generation.

**Shutdown flush.** Pending saves (the debounced active activity) are written out on a graceful
stop: hub-sim SIGTERM, a firmware OTA reboot, or `reboot`. A power cut loses at most the
debounce window.

## 2. One generation counter

- The config has a single saved, monotonic `generation`, starting at 1, which is the record
  generation from section 1.
- Every committed change bumps it.
- Each object's `revision` is the generation of its last change. The same goes for global
  bindings and integrations.
- There are no separate counters and no zeros.
- Restore and reset stamp every object with the new generation, so stale clients always get 409.
- If a backup omits revisions or has revision 0, it's accepted; the stamping replaces them.

## 3. Integrations: one source of truth

- `config.integrations` is authoritative and always complete. The built-ins (`system`, `ble`)
  are always present, put there when the config is loaded or created.
- One resolver, used by both `compileConfig` and `compileDeviceCommand`, maps device →
  integration → adapter. It checks that the integration exists, that the provider type matches,
  that the integration is `enabled`, and that an adapter is registered.
- A disabled integration's devices compile to "unavailable" (`integration_disabled`). Execute
  and bindings report that rather than reaching the adapter.

## 4. API responses tell the truth

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

## 5. Rules added after the second review

These come from the second independent review (10 findings, 3 high). They extend parts 1–4. They
aren't separate patches.

- **Never destroy on uncertain information.** Automatic destructive actions (formatting, writing
  defaults, clearing state) happen only on positive knowledge. Format LittleFS automatically only
  when the partition reads as erased flash (all `0xFF`); otherwise a mount failure means
  `StorageUnavailable` until the user explicitly asks for a format. There's no "already
  formatted" flag in NVS: it was a second, unreliable source of truth.
- **Secondary state follows primary state.** State derived from the config (the active
  activity) is never written while the config isn't `Ok`. A failed restore against a
  Blocked or unavailable config means "unknown", not "none". `StateStore::get` distinguishes
  absent from unreadable, as every record does.
- **The record store owns generations.** Owners never cache the next generation; they ask the
  store. If the store can't invalidate a slot after a failed save, it reports itself unhealthy
  (`ReadOnly`), so no later save or acknowledgement happens on a medium it can't trust.
  Residual risk: if the medium fails exactly while invalidating, an unacknowledged record can
  survive; the hub then reports a storage fault rather than carrying on.
- **Held keys are guaranteed at runtime, not by validation alone.** The engine keeps a ledger of
  holds per button: which adapter, and which release command. When that button goes up (from
  any binding, global or activity), or its sequence fails or is cancelled, or the radio times
  out, or there's a transition, every hold still open for it is released. Static validation
  (holds only on `:down`, a matching release) stays as early feedback for the user. It is not
  the guarantee.
- **One pipeline per operation.** A dry run is the real commit pipeline without the final save:
  the same normalization, built-ins, stamping, size limit and round trip. Every config edit
  runs the same steps in the same order: editable check, revision check, build, round trip,
  save, publish. No legacy wrappers or duplicate validators.
- **Never block across tasks.** The RF/engine task never waits for another task. A reboot is a
  request: httpd does the flush and then restarts. Recovery retries back off exponentially
  instead of running on every request.

## 6. Rules added after the third review

- **Absence needs proof.** A record is absent only when no slot file exists, or every slot
  that exists is one we deliberately invalidated (zeroed header). If a slot is present but
  fails its CRC, uses an unknown format or is too large, and no slot is intact, the record
  is **Corrupt**, never absent: the config goes Blocked and credentials go Corrupt. One
  torn slot next to an intact one still falls back, as before. Accepted cost: if power fails
  during the very first save of a record, the next boot shows Corrupt and needs a reset.
- **Don't present a guess as data.** While the config is Blocked, a backup exports the saved
  record's raw bytes, clearly marked as blocked, rather than the empty runtime config.
- **A destructive action needs its trigger cleared first.** A requested format runs only
  after the request flag has been durably cleared; otherwise the hub doesn't format.
- **Restore and reset never start an activity.** After a restore or reset, the active activity
  is kept if its id still exists; otherwise the hub becomes inactive without running any
  startup or shutdown. The "switch to the next activity" rule applies only to deleting the
  active activity.
- **One owner per medium.** Trust and health (including `ReadOnly` after a failed invalidation)
  belong to the storage medium, so every record store on it sees the same state.
- **One boot path.** "Load config → load credentials → restore the active activity" is a single
  core helper that the firmware and hub-sim both call. A hold is forgotten only once its
  release has completed successfully; a release that fails asynchronously falls back to
  `releaseAll`. Credential refresh backs off the same way config refresh does. The RF task
  never touches NVS.

## Verification bar (before Phase 2 is committed)

1. The disabled round-trip tests are enabled and passing. The crash invariant gets stricter:
   after a failed save the hub always has the old config.
2. The reboot round-trip and random crash tests run at higher iteration counts, plus the
   contract restart phases, in both auth modes.
3. ASan and TSan run several times in a quiet tree (settles the unexplained use-after-free).
4. A second independent `/code-review` (high) has no high-severity findings.
5. Hardware checklist on the dev board, including pulling power during a save and a corrupted
   filesystem.
