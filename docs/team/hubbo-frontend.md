# hubbo-frontend: working notes

Current state, not a log. Update when a piece of work lands.

## Unique hub name (branch `hub-name`; docs/design/hub-name.md)

- A new hub is "Hubbo XXXX" (MAC suffix); hub-sim's and the mock's is "Hubbo 534D" / `hubbo-534d`
  (`DEFAULT_HUB_NAME` in mock/handler.ts). First run and factory reset bring it back; the config
  reset (POST /admin/reset) leaves the name alone. "Hubbo" in UI copy is the product, not the hub.
- Hub is `{name, hostname, hostname_conflict}`; `hostname` is what mDNS really advertises (also
  /status network.hostname). Settings → Hub shows "Another hub on your network already uses this
  name, so this one is at <hostname>.local." when `hostname_conflict`; a rename's reply clears it.
  Not in Setup, by decision. The UI never derives a hostname itself.
- Mock knob `POST /__mock/hub?conflict=1|0`: hostname `<name's>-2`; `hostname_conflict` is derived
  (hostname ≠ the name's), so a rename clears it. hub-sim has no mDNS: the hint e2e is mock only.
- The hint is the Name field's `hint`. Status leaves the hostname out when there's none. Setup's
  Name step: an unread name shows an error with Retry; Continue waits, Skip keeps the hub's own.
- Tests: setup.spec expects "Hubbo 534D"; hub-settings.spec (hint); mock unit test (default + knob).

## HA domains (branch `ha-domains`; docs/design/ha-domains.md)

- New HA domains: scene, script, cover, button, input_boolean, humidifier, select (remote and
  climate dropped by the user; locks skipped). New kinds cover, scene, script (labels from GET /kinds).
- Add device: the Type defaults to the candidate's `suggested_kind` (required, from the hub).
  No name-guessing in the UI any more. Subtitle "Media players, lights, covers, scenes and more".
  No per-kind icons exist (kinds are only a label in a select).
- Params need nothing new: enums and ranged ints already render. Additions: a required `humidity`
  % starts at 45 (other whole % at 35, e.g. a cover position); a number with a `unit` reads with
  it in summaries ("Set position 50%", "Sleep timer 300 s").
- Mock: `HA_DOMAINS` lists the 11 domains. A free candidate's registered commands come from
  `api/fixtures/integrations/ha_main.entities/<entity_id>.capabilities.json` (`Seed.entityCapabilities`),
  so the mock stores the hub's real params. A candidate keeps its fixture `platform` unless
  `PLATFORMS` gives one. The mock's own candidates (Denon, Patio Fountain) carry `suggested_kind`.
- Tests: e2e `ha-domains.spec` (mock and hub-sim: add a cover with its suggested type, position
  field; humidifier modes and humidity 45; select options; a cover step "Set position 20%");
  mock unit test registers each new entity; describe/activity units.
- The wizard's HA power mapping (HA_NAMES) leaves the new domains out, by decision.
- HA devices show their entity id under the name (Devices list, device page header) via the shared
  `EntityId` (components/common.tsx); Add device's candidates and pick use it too. Bluetooth: nothing.
- Gate 2026-11-13 (after /simplify, review, shade → cover): green. Unit 161; mock 137;
  `--sim --auth` 94; `noauth` 4.

## Agent docs (branch `agent-docs`; mock parity for the contract)

- `mock/docs.ts`: read at mock start from the repo (never copied): api/openapi.yaml as JSON
  (js-yaml, CORE_SCHEMA: dates stay strings), docs/agent-api.md and docs/llms.txt (exact bytes;
  llms.txt is the source of truth for /llms.txt). `sendDoc` is core's: gzip only on
  Accept-Encoding gzip, always `Vary`, sha256 ETag + 304, `no-cache`. Built once per process
  (`loadDocs` caches), not per createMock. Routes (public): GET /api/v1 and /api/v1/ (discovery/index.json,
  version from /status), /openapi.json, /agent.md. node.ts serves /llms.txt, and `unknownPath`
  (handler.ts) for /api, /api/ and any non-v1 /api/ path.
- `error.docs = /api/v1/agent.md` as core: on the `unauthenticated` 401s (no credentials; not a
  wrong password's `invalid_credentials`) and on unknown-path 404s; never a 405 or a missing id.
- Vite dev proxy sends `/api` and `/llms.txt` to a hub target. index.html has a hidden
  `rel=alternate` link to /llms.txt and an API comment for agents fetching `/`.
- Dev deps: js-yaml 4.3.2, @types/js-yaml 4.0.9.
- Tests: mock unit tests (docs served, gzip/304, hints); e2e `agent-docs.spec` (through the dev
  server: /llms.txt, index, guide, /api 404 hints, the link in index.html), mock and hub-sim.

## Hub name and restart (branch `hub-name`; simplified, tests caught up; review next)

- **Settings → Hub** is the first tab and what /settings opens to (`components/admin/HubSettings.tsx`):
  - Name: a field + Save. `lib/hub.ts`: `trimHubName` (spaces only, like the hub: a tab is a
    control character it refuses), `hubNameProblem` (1–32 characters), `saveHubName`. A 422 detail
    at /name shows inline. After a save: "Paired devices may show the old name until they
    reconnect." No preview, by request.
  - Restart: confirm "Restart the hub?" / "You’ll need to sign in again." → POST /admin/restart →
    "Restarting…" → the app reopens (sign-in). 409 `update_in_progress` → "An update is running.
    Try again when it’s done."
- **Setup step "Name"** (Setup.tsx `HubStep`, step 2 of 6, after Account; `#/setup/hub` holds it
  like Network): "Hub name", shown once GET /admin/hub has loaded (Continue waits for it). Setup
  holds the name and saves it on Continue, only when changed; a refused save doesn't block (toast:
  change it in Settings → Hub). Skip moves on. Done lists the name. No config lock on the name
  (it isn't config), in Settings or setup.
- **Restart wait** (`lib/restart.ts`): `followRestart(back, onSlow)` polls the public session and
  reopens the app; `backFromRestart` (went down and answers, or the session is gone, or the no-auth
  simulator); `RESTART_SLOW_MS` + `NOT_BACK` shared with FirmwareUpdate; FactoryReset uses it too
  (and now shows the slow line).
- **Password managers**: the shared `Input` marks every non-sign-in field (anything but
  `username` / `current-password` / `new-password` autocomplete) with autocomplete=off,
  data-1p-ignore, data-lpignore, data-form-type=other. Callers don't add them.
- **API**: `Hub` is the generated `S['Hub']`. Mock: GET/PUT /admin/hub as core (spaces trimmed,
  `required` / `too_long` (fixture) / `invalid_name`, unknown fields 422; hostname derived from the
  name, ASCII-only lowercasing like core; control characters checked before length; also /status
  `network.hostname` and `hubbo.name`); first-run hub is "Hubbo". One `restart`
  timer serves POST /admin/restart (sessions end) and the factory reset (erase), with `onBack`.
- **Tests**: e2e `hub-settings.spec` (tab, attributes, trims, refused name inline, rename +
  hostname; restart → sign-in, or Activities on the no-auth simulator), setup.spec (Name step:
  rename + blank, default kept), a11y (/settings/hub). Units: lib/restart.test, lib/hub.test, menu.
- **hub-sim**: core's sim reset now forgets the hub name. backup.spec is an `app` spec (mock and
  `--sim --auth`); on no-auth hub-sim its factory-reset tests can't pass, and the gate never runs them
  there. setup.spec under `--auth`: hub.ts factory-resets a set-up hub-sim for a first-run test
  (its `--data` keeps the admin across `--reset`), so every setup test runs in one pass.

## Web polish (branch `web-polish`; review fixes in, gate after them)

- **What's on**: no per-page activity chip. The nav's `Brand` shows `NowOn` (`role="status"`, dot +
  running activity or Off) under "Hubbo", sidebar and phone header. Desktop main `md:pt-8`.
- **⋯ menu** (`ui/menu.tsx`): hands its trigger the menu props; closes on outside pointerdown, Esc
  (focus back on the trigger), Tab, item chosen; arrows/Home/End. On open it focuses the checked
  item, else the first (callers never autoFocus); `onOpenChange` is held in a ref.
- **One shared remote editor**: `BindingsEditor` takes `set: BindingSet` (`{source:'global'}` or
  `{source:'activity', globals}`: an activity always gets the globals, so legend, amber marks, Global
  tag + Change are identical on both hosts) and `selected: string | null` (null → the set's first
  button: select / tv). A `selected` change from the page (ActivityEditor's error jump) opens the
  phone sheet too. Both hosts wait for the globals before showing the editor.
- **One shared host** (`hooks/draft.ts` `useDraft(store, server, catalog)` + `components/
  ConflictDialog.tsx`): draft/saved, validation (errors from the first Save on), Save + Cmd/Ctrl+S,
  conflict (fetch theirs; Keep / Load theirs / Save mine), beforeunload guard. Stores:
  `activityStore(id)` (whole activity, `toDraft`) and `globalStore`. The draft resets only when the
  store key changes, or the saved copy's revision changes while clean, so refetches never wipe edits;
  a save that lands after a switch only updates its own set. Retry uses `retryFailed(...queries)`
  (useCatalog has `refetch`). Pages show ErrorState only when nothing has loaded.
- **Remote page** (`pages/Remote.tsx`, `/remote` Global, `/remote/:activityId`): one route, never
  remounted; while the next set loads the last draft shows dimmed. Picker disabled while saving; a
  dirty switch asks "Discard unsaved changes?" (confirm; the user can veto). An activity here edits
  the whole activity draft (validateActivity), same as the activity editor.
- **Masking rule**: `bindsButton` (lib/activity): a button is bound only if a trigger has ≥1 action;
  both the remote's inherited marks and the panel's `owns` use it. Inherited tooltip keeps the key
  name: "TV, Global: Press → … · Long press → …".
- **Colours**: amber means Global (`--remote-global` on the remote, `--global` page bits); Long
  press dots violet, raw Up sky blue. `SourceLegend` only for an activity set.
- **Phone**: `useWide` (hooks/media.ts, one module-level MediaQueryList; true without matchMedia).
  Below md, a tapped key opens `ui/sheet.tsx` (55dvh, Done, Esc, focus back to the key, no scrim or
  gestures). Gear menu (theme radios + Settings) is phone-only; tab bar has five sections.
- **Tests**: `e2e/remote-page.spec.ts` (picker in place + Back, activity save, dirty-switch confirm,
  global conflict, nav status, sidebar Settings/theme, tile menu; phone tab bar, gear, sheet).
  activity.spec (inherited keys), conflict.spec (activity), a11y (/remote, /remote/<act>). Units:
  menu (focus on open), RemoteSvg (48 keys, one tab stop, arrows, Enter; global marks, tooltip),
  describe (actionText), activity (`bindsButton`).
- **Open**: no e2e for the phone sheet opening on an error jump (hard to make a local binding error
  in the mock).

## Web admin UX review (branch `remote-editor`, simplified, tests caught up)

Quick user-driven changes on hub-sim (the user's :8790 and :5180; don't start servers there). Tests
were deferred while iterating and are now caught up.

- **Devices**: one flat list sorted by name (no per-integration sections). Each row: platform icon
  (lit when live: Bluetooth connected / HA available; `role="img"` aria-label like "Bluetooth,
  connected"), the name, a subtitle only when action is needed (Not paired, Ready to pair, Needs
  re-link), chevron. An HA device also shows its entity id (EntityId). No availability badge.
  Specs caught up (devices, ble device-list, ha stale): rows found by href + icon aria-label.
- **Device detail**: one column: Device pane (Name + Save, then a Type select from GET /kinds that
  saves on change via PUT kind+revision, then HA refresh/re-link, Remove), Bluetooth pairing, then
  Commands. No page subtitle. Bluetooth "Forget" is now "Unpair" (outline sm, like Pair; confirm
  "Unpair it?"). `useKindLabel` removed (unused).
- **Commands by name** (`lib/commands.ts`, pure): Bluetooth's press/key/usage families become the
  device's own commands (`press:<name>` → "Home", "Up", "Play/Pause" via `commandName`), then
  "Keyboard key…" (key_tap) and "Custom code…" (usage_tap) under More; HA/system commands as they
  are, minus hold-effect ones. `choices`/`choiceOf`/`actionFor`/`choiceParams` drive the device
  page's Commands (Test sends a tap; no "Buttons only") and `CommandPicker` (bindings and steps).
  `friendlyName` makes `commandLabel` read "Home", "Arrow up down", "held 2 s". Key pickers group keys
  (`groupKeys`: Navigation, Letters, Numbers, Function keys, Media, Symbols, Other) in ParamForm.
  On/off commands (`toggleParams`: one required bool, or only optional bools) split per bool into
  `set:<id>:<param>:<true|false>` ("Mute"/"Unmute", "Shuffle on", "Night sound off"); an action
  setting several bools stays raw. `commandTitle` humanizes raw-id labels (`sonos.set_sleep_timer` →
  "Set sleep timer"); `paramLabel` does the same for params. Services whose prefix isn't the
  entity's domain (`domainOf`) go under More (device page heading, picker optgroup). Device page
  rows are flat: label, inline controls (`ParamForm inline`: dropdowns keep sr-only labels; number fields get a visible short label beside them via `Field inline="beside"`, no placeholder), Test.
  Percent params (`percentScale(commandId, p)` in lib/activity): a 0–1 number (HA volume level),
  `light.*` 0–255 `brightness` (stored round(pct×255/100)) and `light.*` ±255 `step` (signed,
  −100%…+100%, no 0). Selects in 5% steps, off-grid values kept as their own option; defaults
  35% / +10%; summaries "Set volume 35%", "Change brightness +10%" (no parentheses).
  ParamForm: primary params (required, or a sole optional one with no default: `drawerParams`)
  show in place; other optionals sit in a drawer (muted, sectioned) behind a chevron that's always
  there for such a command ("More settings for <command>", aria-expanded; a dot when a value is set
  and differs from its default). Device rows: name, then a right-aligned group of primary controls,
  Test and a chevron slot always reserved (a spacer when none), so Test/chevrons form columns; the
  test result shows under the name (`TestButton onResult`, `TestResultText`). Slots `before`/`after`;
  pickers: a "More settings" chevron button under the fields. Opens initially when a stored action
  has settings; each set drawer field has a quiet "Reset" (to default/unset).
  Catalog labels go through `sentenceCase` (utils): only a fully Title Case label changes
  ("Sleep Time" → "Sleep time"); mixed ones stay as HA wrote them ("Join Sonos group").
  Fan speeds (`fanSpeeds`: a `fan.*` `percentage`, unit "%", step > 1): a "Speed n of N" select,
  N = round(100/step), speed n stored as floor(n×100/N); an off-list value shows as the speed HA
  runs (ceil) without being rewritten; default speed 1; summaries "Set speed 2 of 3". Other
  required "%" ints default to the allowed value nearest 35%, never 0 (`nearPercent`).
  Percent and fan-speed params share one rule, `numberChoices(commandId, p)` (lib/activity):
  options, picker label and summary phrase; ParamForm, describe and fieldLabel all use it.
  Device page rows are keyed by choice + their param spec (`rowKey`), so a refetch that changes a
  command's spec re-seeds the row; Name and Type save one at a time (`save` in DeviceAdmin, the
  reply written to the device query so the next save has the latest revision).
  HaCandidates (Add and Replace) asks discover for `available=true` (supported, unregistered,
  filtered before paging, so Replace's own entity is hidden too); the mock does the same and
  answers a bad value with 422 wrong_type at /available.
  A param `description` shows only as a tooltip: `Field info=` / `ui/info-tip.tsx` (info icon by the
  label, or after the control when the label is sr-only; hover/focus/tap, Esc closes, fixed to the
  viewport). Labels: the catalog's
  command/param `label` as-is, humanized only when missing; `fieldLabel` adds the `unit`
  ("Speed (s)"), with `KNOWN` (activity.ts) as a fallback for `color_temp_kelvin` / `transition`;
  a fallback label drops a unit suffix the unit already says (`hold_ms` → "Hold (ms)").
  Mock entity details: `skipped_services` are `{service, reason}` (`skip()` in mock/handler.ts);
  the mock offers `sonos.snapshot` (with_group, default true) to show a modifier.
  `defaultParams` fills required params only: an optional param with a default is a modifier, not
  sent unless changed, shown pre-set to its default (no "Not set").
  On/off split (`toggleParams`): one required bool (other params follow as usual), or optional bools
  with no default (each choice sends just its bool). Optional bools with a default don't split.
  Enum option labels go through `paramValueLabel` → `titleCase` (utils; words with a capital or digit
  kept: "HDMI 1", "eARC"); values unchanged. The catalog has no option labels to prefer today.
- **Binding rows** (`BindingsEditor`, branch `button-press`, Harmony-style, gated): a Press row (no
  tap/hold choice) and a Long press row, raw Down/Up/Tap under "Advanced" (forced when
  `readButton` can't fold). Press with no long press is the mirror: `down` press_down / key_down /
  usage_down and `up` its release (an HA command: one send on `down`; Hubbo's own: `tap`). With a
  long press bound, or for a timed profile command (BLE `hold_ms`, e.g. Control Center: only a
  `press` times it; `Catalog.timed` from device details, `isTimed`), Press is `tap`; "Sends on
  release" shows only with the set's own Long press. `writeMain`/`writeLongPress(…, timed)` are the
  one encoder; the wizard uses them too (`NamedCommand.timed`; buttons.json has no `hold`).
- **Globals mask per button** (core's rule): a button the set binds at all inherits nothing; an
  untouched one shows "Global: …" rows with Change (opens on the global's action; Apply unchanged
  binds nothing). A truly unbound row's Bind opens empty (no device or command; Apply off until a
  command is picked). Only Add step guesses (most-used device of that sequence).
- **Remote**: the selected key has one thick pink-500 border (`--remote-selected`) in place of the
  teal bound one (pips and tint stay); focus stays the dashed ring; `aria-pressed` marks it.
- **Pickers**: Device | Command | the primary param in one row of content-sized selects (ParamForm
  `lead`, field mode `fit`; always through ParamForm so focus survives null → set), Device alone on
  a phone; the drawer below (opens per command, `openFor`). `DeviceSelect` is shared: no "None"
  (a disabled "Choose…" only when nothing is picked), "Unknown device" for a removed id, "Hubbo"
  last when it has commands there (`hubboCommands`: Switch activity and Off in bindings; nothing
  in steps). New steps start on the sequence's most-used device (`start.ts`).
- **Steps**: drag by the grip with pointer events (mouse and touch; `[data-grip]`, `touch-none`),
  an insertion line in the gap (top/bottom half of a step); Alt+↑/↓ on a focused step; no move
  buttons. Duplicate copies below (off at the limit). Add wait adds 500 ms. Editing a wait opens
  only "Wait (ms)" (step 100); a BLE wait_connected only its timeout in seconds (`TimeField`:
  validity from mount, bounds from the catalog or `WAIT_MS`/`WAIT_CONNECTED_MS`). Durations read
  "500 ms" under 1 s, else "1.5 s" / "2 s" (describe.ts `duration`).
- **Activity editor**: the name edits in place (Enter/blur saves, Esc restores); no Check button
  (Save validates; the client has no validateActivity; the mock still serves the endpoint).
- **Tests**: e2e `bind(page, button, 'press' | 'long press', device, command, key?)` picks commands
  by name ("Keyboard key…" + key); drags use the grip. activity.spec covers the step dialog
  (no None/Hubbo, Duplicate, wait in ms) and per-button inheritance. Units: `commands.test`
  (mirror ↔ tap, timed, Hubbo tap), `describe.test` (durations), `activity.test`
  (hubboCommands, mostUsedDevice), `start.test`, `SequenceEditor.test`, `DeviceSelect.test`,
  wizard `build.test`. Component tests use hand-made catalogs (fixtures don't load under jsdom).

## Device profiles and the activity wizard (committed on `device-profiles`; docs/design/device-profiles.md)

- **Add device** (`AddDeviceDialog`): one list. Built-in profiles (GET /profiles, searchable by
  manufacturer and model; shown when their platform's integration is on) plus "From Home
  Assistant". A profile → name (defaults to the model) and type (from GET /kinds) → POST
  {profile_id, name, kind} → the device page (Bluetooth pairing card). No hard-coded kinds:
  `useKinds`/`useKindLabel` (hooks/data).
- **Wizard**: `pages/ActivityWizard.tsx` at `#/activities/new` (devices → watch → volume → inputs →
  power → review; steps with nothing to ask are skipped; Back, the progress bar and the review's
  "Change" links go to any step). Save creates an ordinary activity and opens the editor. Every new
  activity goes through the wizard: there is no empty editor, and `ActivityEditor` only edits an
  existing id. E2e that builds by hand uses `openNewActivity` (editor.ts: POST an empty activity,
  open it) and `saveActivity`. Step 1 is Name, then an "Included Devices" heading (`#step-title`)
  over the checkboxes; other steps have a short heading and a one-line blurb. Review shows no button
  map: answer rows (Watching with, Volume, Inputs; Change only for asked steps), then the Start and End
  steps (toggle-only devices noted under Start). Buttons are still filled into the activity. Bluetooth devices'
  detail (GET /devices/{id}; the list is light) loads once when the wizard opens via
  `useDeviceDetails` (hooks/data, seeds `devices/<id>` like `caps-all` seeds `caps/<id>`), behind
  the Skeleton; HA devices need only the list and capabilities. Ticking a device only filters,
  never fetches (a per-selection key made Continue flash). `ActivityDraft` now always has id and
  revision (the editor only edits stored activities); create/validate take `ActivityInput`.
- **Builder**: `lib/wizard/build.ts`, pure and unit-tested. Devices become `WizardDevice`s (HA from
  capabilities via `haNames.ts`; Bluetooth from `DeviceDetail`). Rules from `profiles/buttons.json`
  (repo root, imported at build time via `rules.ts`). HA first, then Bluetooth; Bluetooth: `wait_connected`
  (15000) before its first step, power on as `press` steps; on/input delays; `key_delay_ms` wait between steps to
  the same device; HA turn_on + input gets a 3000 ms wait; HA mute = press mutes, hold unmutes;
  toggle-only power gets no step; holds use press_down/press_up.
- **Key picker**: `commandGroups`/`groupOf` (lib/keys) group Bluetooth commands as Remote commands,
  All keys, Custom code (hidden behind "Show custom codes"). Waits (`effect: wait`) are offered only
  in sequences. The `press` param's label "Remote command" comes from the capabilities, so e2e
  matches `getByLabel('Command', { exact: true })`.
- **Mock**: devices stored in the detail form, `light()` for the list, `platform` on responses but
  not in backups. /profiles, /profiles/{id}, /kinds from api/fixtures/profiles. `addFromProfile`
  copies the profile (unknown_profile, unknown_kind, the 4-device limit). Bluetooth capabilities =
  the seeded Bluetooth device's commands with the profile's names as `press` choices (`bleCaps`), so
  the key table isn't hard-coded. wait_connected: execute → sequence_only fixture; in a binding →
  `sequence_only` (shared validator). Restore adds platform and rebuilds Bluetooth capabilities.
- **e2e**: `wizard.spec` (HA TV + receiver; plus a Bluetooth streamer), `ble.spec` adds each
  built-in profile through the dialog. Mock and hub-sim --auth pass. storage.spec flaked once in a
  full mock run and passed alone.
- `/simplify` done. The wizard reads picked devices through the shared `devices/<id>` cache;
  the builder reuses `waitAction`/`isWait` (lib/activity). Skipped: the mock's hard-coded
  Bluetooth platform in `addFromProfile` (the only profile platform).
- A Bluetooth device's `connectFirst` (wait_connected) goes before its first startup step,
  whatever it is (power on or an input), once; devices with it run after the others.
- Review "Change" links go to the first asked step for that section, or don't show.
- Open: the full gate; phase review.

## Several Bluetooth devices (committed on `multi-bluetooth`; docs/design/multi-bluetooth.md)

- `ble` is the radio: its card shows only health ("Connected", or "Bluetooth isn't working").
  Each host is a device: Add device → Bluetooth → name (no discovery). At most 4
  (`BLE_MAX_DEVICES`); a fifth is 422 `too_many_devices`, shown as the hub says it.
- Device page: `BluetoothPairing` card. Status, plus Pair, Cancel while the window is open, Pair
  again (with a confirm), and Forget. It polls `GET /admin/devices/{id}/pair` every 1 s while
  open. A window that ends `not_paired` (not cancelled) says "Nothing paired. Try again." A
  re-pair that's cancelled or runs out keeps the old host.
- Status everywhere: `Device.bluetooth` (`BluetoothStatus`), shown by `bluetoothLabel` and
  `bluetoothDot` (lib/health): Not paired, Ready to pair · N s, Connected, Not connected. A key for
  a host that isn't connected is 200 `failed` `adapter_unavailable`.
- Mock: per-device `bleHosts` and one `blePair` window; `live(d)` adds `bluetooth` and
  `availability` to GETs, and they're never stored or in backups (restore refuses them as
  `unknown_field`, like PUT/POST). Controls like hub-sim: `ble/bond?device=`,
  `ble/link?device=&up=`, `ble/radio?ready=`. e2e helpers are `bondHost`, `setHostLink` and
  `setBleRadio`.
- Mock follows the hub's bond rules: `pruneBonds()` after delete, reset-configuration and restore
  drops bonds (and a window) for devices no longer in config; a restored device that still has a
  bond keeps it. Pairing the device whose window is open is 202 with its status; 409 `busy` is
  only for another device's window. The hub's 503 `busy` (full queue) isn't simulated.
- Key names: `keyLabel` and `paramValueLabel` (lib/keys) for the key picker and the action text.
- `errorMessage(e, {details})` skips a detail that repeats the top message.
- `/simplify` done twice. The mock answers fixed hub errors from `api/fixtures/errors/` via
  `fixtureError`; `unknownFields()` builds unknown_field details; `humanize` (lib/utils) is the one
  snake_case → "Sentence case" helper. Skipped: rendering the card from `device.bluetooth` to save
  the one pair GET (it would flicker after a window ends while the device refetches); a union
  `Pick` type; a `deviceStatus()` helper for the list (two small checks); and option labels from
  the hub (an API ask that would replace `paramValueLabel`'s name check).

## Polish round 1 (committed on `polish`; docs/design/polish-ux.md)

- **Routes, no redirects** (the product never shipped; old routes are deleted): `/` Activities
  (home), `/activities/global|new|:id`, `/devices[/:id]`, `/integrations`, `/status/:tab?`
  (overview, backups, firmware, diagnostics), `/settings/:tab?` (account, tokens, remote, network,
  reset). An unknown tab renders NotFound. Nav: Activities, Devices, Integrations, Status, Settings.
- **Home** (`pages/Activities.tsx`, eager): tiles are `aria-pressed` buttons that start the
  activity; compact one-line rows (one column on a phone), the running one filled with an "On"
  pill; Off is in the header (disabled, still visible, when nothing runs); edit/duplicate/delete sit behind
  "More for <name>". e2e locators on tile names need `exact: true` (they clash with "More for").
- Dashboard is gone; `pages/Status.tsx` holds overview and diagnostics, `admin/Backups.tsx` the
  backup/restore that was in Settings. Appearance tab removed. Resets live in Settings → Reset.
- **Copy rules** (the user's): like regular apps, light and short; never explain the
  implementation or the dev process, never assume our setup (Apple TV, HA, our remote). The
  "hands off the UI" rule is lifted for the polish pass.
- Shared: `Facts` (common.tsx), `EVENT_VERB` (lib/remote), `plural` (lib/utils),
  `isUserIntegration` and `KIND_LABEL` (lib/health). `ErrorState`/`NotAvailable` lost `what`.
- **Integrations**: the card header is the one place the state shows (`state-<id>` test id;
  /integrations polled every 3 s, since /status has no `detail`). Off shows only "Disabled";
  Bluetooth shows its detail alone; others `state · detail`. `HaConnection` shows
  only its alerts there; Setup passes `showState` for the state line.
- Device page: service ids show only in an opened command; "Change entity…" (was Re-link). The
  remote endpoint shows no radio status, only an alert when the radio isn't working.
- Firmware refusals show the hub's message (now plain). The mock's strings match the hub's:
  BLE detail, "Not set up", "<id> isn't in Home Assistant", "Used by …", the firmware refusals.
- **Screenshots**: no before/after shots unless the lead asks. `e2e/shots.spec.ts` runs only when
  `build/shots/.take` exists; it writes `build/shots/current/<desktop|phone>/`, moved by hand to
  `before/` or `after/`. `build/shots/index.json` holds captions and old → new names.
- API messages were made plain by the API and core agents (round 2).

## State of the web admin (Phase 5, committed on `phase-5`)

- React 19 + TS + Vite + Tailwind, shadcn-style components in `web/src/components/ui`. wouter with
  hash URLs. Our own small query cache in `src/api/hooks.ts`: `useQuery(key, fn, { pollMs,
  keepPrevious })` (one timer per key, paused while the page is hidden), `invalidate` (prefix),
  `setQueryData`, `getQueryData`.
- Every request goes through `src/api/client.ts`. Types alias the generated `schema.gen.ts`
  (`src/api/types.ts`). After an `api/openapi.yaml` change run `bin/hubbo web gen:api`.
- Phases 2–3: backup/restore (opaque blob), blocked/storage banners, conflict dialogs; HA settings
  with a live connection line (token write-only); async discovery in `HaCandidates`; register,
  stale banner, Re-link and Refresh on the device page; remote endpoint and "Scan from Hub".
- Phase 4 (docs/design/phase4-web-admin.md, sections 1–4):
  - **Wizard**: admin, network, remote, Home Assistant, done (no devices or activities). It resumes
    from hub state only (`useResumeStep` in `Setup.tsx`, reading /status: no admin → admin;
    unpaired → remote; HA not_configured → HA; else done). `#/setup/<step>` opens a step. Creating
    the admin goes to `#/setup/network` (hub state can't tell Network was seen). The resumed step is
    fixed once known. Once finished, the wizard only opens via `#/setup` or Settings.
  - **Errors after a save attempt**: the `ShowErrors` context in `ui/input.tsx` (Field, Input,
    Select obey it; default shows). Forms provide `false` until the first save/submit. Save stays
    enabled; an invalid Save just shows the errors.
  - **Selects show what's stored**: unset is "Choose…" (required) or "Not set" (optional; picking
    it clears). A stored value that isn't an option is its own "<value> (not available)" option; a
    ref shows its raw id while activities load. `defaultParams` leaves an activity_ref unset.
    Unit tests: `ParamForm.test.tsx`.
  - **HA Save** (wizard and Integrations, one component): Save stores the config, calls onSaved,
    invalidates status; it never reads /status itself, so a status failure can't be a save error.
    `HaConnection` remembers the pre-save state and shows "Connecting…" until the state moves or
    `HA_APPLY_GRACE_MS` (2 s) passes (the HA task applies settings on its next pass, so an early
    read is the OLD connection). Polls 1 s until the outcome is in, then 3 s; 20 s → "still trying".
  - **Remote endpoint** (wizard and Settings, one `RemoteEndpoint`): the fields always show the
    stored endpoint (null edit state = stored), so a save never blanks them. Save says "Saved.
    Listening on channel N, address …" and mounts `PressCheck` (also on a scan's "Remote paired"):
    it polls /remote/status every 500 ms through the cache and shows "Got it: <Button> pressed" per
    new `last_event` (`isNewEvent` in `lib/remote.ts`: younger, or another button/kind; the first
    read is the baseline), "Waiting for a button press…" before, and a hint after 30 s.
  - **HA entity details** (§3b): picking an entity loads its details (`loadEntityDetails` in
    `lib/ha.ts`, Retry on failure), which register, re-link and refresh need (409
    `details_required` otherwise). `EntityPreview` shows them. Details with `registered_device_id`
    (the same entity, or same unique_id+platform after a rename) show "Already added as <name>"
    with a link instead of Add; a register 422 `duplicate_id` reloads the details to show the same.
  - iPhone: `keepPrevious` keeps the HA search box mounted while a search loads (the keyboard
    closed per letter). `Select` draws a real ChevronDown. PageHeader truncates only string titles
    (it clipped the activity-name focus ring).
  - Duplicate asks for the name. Steps reorder by drag (mouse) and Alt+↑/↓; phones use the buttons.
  - Re-link refused when it breaks an activity: 422 paths by activity index; `haDeviceError` turns
    them into "Watch Apple TV, startup step 4: …".
  - Accessibility: axe (WCAG 2 A/AA, light and dark) clean of serious/critical on the main pages;
    the test waits for CSS transitions (axe measured mid-fade). Phone keys have ≥ 24 px hit areas.
- Phase 5 (docs/design/phase5-appliance.md §1–2):
  - **Auth trim**: no CSRF token (client sends none; the hub checks Origin → 403 `csrf_failed`),
    no 429, no `corrupt`. `CredentialsProblem` has one screen (`unavailable`: restart or wipe).
    Credentials are read once at boot, so a session outlives storage going down: `storage.spec`
    now runs under `--auth` too, and `storage-auth.spec` and the `last` project are gone.
  - Setup and Login start with an **empty username** (autofocus there; `autoComplete` username /
    new-password / current-password). The mock's set-up admin and e2e `ADMIN` are the login
    fixture's (`hubbo-owner`, like api/tools/contract.py).
  - **Firmware** (`components/admin/FirmwareUpdate.tsx`, Status → Firmware): version and board;
    a .bin picker that reads the file's marker (`lib/firmware.ts readMarker`) to say "Hubbo vX for
    <board>" (the hub decides; nothing blocks client-side); `api.uploadFirmware` is XHR for
    `upload.onprogress` (the browser sets Content-Length). After the 202 it polls GET
    /admin/firmware every 1 s: writing % → rebooting → network errors/502/504 = "Restarting…" →
    idle, or a 401 = the hub restarted and sessions are gone (the app shows Login). Module memory
    `pending` = {from, expected: the file's marker version}; after the sign-in the section remounts
    and polls again for a fresh answer. "Update installed" only if the running version is the
    expected one; otherwise "The update didn't take" (a reset mid-write or a crashing image boots
    the old one, and the RAM-only `failed` state is gone). Every 4xx/503 is shown as "Nothing was
    written". A `failed` state shows "still running X". A reload mid-update picks the poll up
    again. `run` (a ref counter) lets only the latest upload/watch update the section.
  - **Mock firmware**: `mock/firmware.ts` builds real-layout fake images (`buildFirmwareImage`,
    used by unit tests and e2e) and checks 0xE9, the marker, the board and the SHA. Like
    SimulatedFirmwareUpdate: 2 s write, 1 s reboot, in hub time = `/__mock/advance` plus real
    time × `timeScale` (default 1/3; e2e env `HUBBO_MOCK_TIME_SCALE=0`; the factory reset uses it too). The second
    half of the reboot drops connections (`status: 0` → socket destroyed); at its end the
    version changes and every session is cleared. `/__mock/firmware?fail=&ready=` mirrors
    `/sim/firmware`; mock-only `?revert=1` reboots into the old version.
- **Backup & reset** (Status → Backups; Settings → Reset):
  - "Reset configuration" (POST /admin/reset, `api.resetConfiguration`): devices, activities,
    global buttons and integration settings go; login, tokens and pairings stay. Typed `reset`.
  - "Factory reset" (`components/admin/FactoryReset.tsx`, POST /admin/factory-reset): erases
    everything; typed `erase`. After the 202 it shows
    "Erasing… the hub is restarting", polls the public /auth/session (not /status: sessions are
    gone) until `fresh`, then reloads into setup. Refusals show "Nothing was erased".
  - After a restore, if ha_main has a URL but no token, a notice links to Integrations (backups
    never hold the token; the hub keeps a stored token only when the restored URL is the same).
  - Mock: restore applies ha_main settings with that token rule; factory reset is down for 1000 ms
    of hub time, then a first-run hub (no admin, sessions, tokens, remote, config; BLE unbonded);
    `/__mock/factory-reset?ready=0` mirrors `/sim/factory-reset` (503).
  - Both typed confirms use `TypedWordField` (common.tsx) and `typedMatches` (lib/utils.ts);
    `sleep` is shared from lib/utils.ts too.
- Bundle: ~155 KB gzipped (`bin/hubbo web build` prints sizes).

## Modes

- **Mock** (default): `web/mock` runs inside Vite, seeded from `api/fixtures`. It's the spec in
  TypeScript, so keep it faithful; `mock/handler.test.ts` holds its tests. Env:
  `HUBBO_MOCK_LATENCY`, `_FIRST_RUN=1`, `_AUTH=1`, `_PAIR_MS`, `_DISCOVER_MS` (e2e sets both to 0).
- **hub-sim**: `HUBBO_API_TARGET=http://127.0.0.1:8780` proxies to `bin/hubbo sim serve` (real
  core with fakes: fake HA at `http://homeassistant.local:8123`, token `fake-ha-token`).
- **Dev server**: :5180 proxies to a local hub-sim, not the board (`bin/hubbo sim serve --port
  8790 --data build/sim-dev --auth`, then `HUBBO_API_TARGET=http://127.0.0.1:8790 bin/hubbo web
  dev`). Never start another on 5180; port 8780 is reserved for the e2e sim; e2e uses its own Vite
  on :5181 (:5182 against hub-sim). Use the board only when hardware is truly needed.

## Mock controls (never in the real API)

- `POST /__mock/reset` body = `ResetFlags`: `firstRun`, `requireAuth`, `configState`,
  `configDamaged`, `startupRejected`, `authState` (unavailable), `discoverFails`, `scanNeverFinds`.
- Mirroring hub-sim's `/sim/…`: `factory-reset?ready=`, `firmware?fail=&ready=`, `storage?health=`, `ha?state=|remove=|auth=&connect=`,
  `ble/bond?device=`, `ble/link?device=&up=`, `ble/radio?ready=`, `radio/found?channel&address`,
  `advance?ms` (also runs down the Bluetooth window and entity details), `remote/<button>/<press|release|tap>` (only records
  `last_event`; no bindings run).
- Mock only: `ha?rename=<id>&to=<new>` (the old id gone; `renamedFrom` makes the new one's
  details name the old device, and registering it is 422 `duplicate_id`, like the hub).
- Entities: the fixture's `demo` entities get realistic platforms (sonos, denonavr, webostv, hue);
  services, skipped ones and unique ids come only with the details. Shared with hub-sim's fake HA:
  YAML-only `switch.patio_fountain` (no platform/unique_id) and a free `media_player.denon`.
- `api/contract.sh mock mock-auth` runs the API contract against the standalone mock. The Node
  path parses bodies itself (413 before parsing, 415, 400 `malformed_json`, depth 8; 12 for
  restore). Admin/token routes before setup are 401, like the hub.

## e2e (Playwright in Docker, `web/e2e`)

- `e2e/hub.ts` is the harness: the `hub` fixture resets the target per test; `test.use({ reset })`
  asks for a state and the test skips where the target can't produce it. Target-neutral helpers:
  `connectHa`, `setHaState`, `fakeHaNextConnect`, `letHaConnect`, `setBle`, `hubAnswersScan`,
  `advance`, `setStorage`, `registerHa`, `removeHaEntity`, `discoverHa`, `api(page)`, all over
  `control(page, path)` (`/sim/<path>` or `/__mock/<path>`).
- Projects: on hub-sim first-run → seeded → app (one shared hub, `workers: 1`). On the mock no
  dependencies, `fullyParallel`, 4 workers: each worker sends `x-hubbo-mock-hub: <parallelIndex>`
  (hub.ts overrides `extraHTTPHeaders`) and `mock/node.ts` keeps one mock per header value (none
  = the dev mock). `phone` (390×844, touch, mock only) runs activate, status, remote, ble and a11y.
  `board` is read-only against a real board (`HUBBO_E2E_BASE_URL`),
  keeps no trace/screenshot/video, and **the normal projects must never point at a board**.
- A file filter is a substring (`ble` matches `unavailable.spec.ts`; use `ble.spec`). Project
  regexes are anchored (`board` once matched `dashboard.spec.ts`).
- **One whole pass per command** (foreground, `timeout` 600000; see "Testing without waiting
  around" in CLAUDE.md). While working, run only the specs a change touches. The phase gate is
  `bin/hubbo gate web`: check, build, then
  - mock, desktop and phone (~100 s, 4 workers): `bin/hubbo web e2e`
  - hub-sim `--auth`, desktop (~3.8 min, the floor): `bin/hubbo web e2e --sim --auth`
  - hub-sim no-auth, only the `noauth` project (~15 s): `bin/hubbo web e2e --sim --project=noauth`
  If a long run moves to the background, wait for its completion notice; don't split it.
  `--sim` builds hub-sim, starts it on fresh data on :8780, sets `HUBBO_API_TARGET` and removes
  it afterwards; never start a sim by hand. A single spec: `bin/hubbo web e2e --sim [--auth] <file>`.
- Projects against hub-sim: no `phone` (layout and touch are the mock's job); `noauth` (no
  dependencies) matches the specs that behave differently without auth: firmware (no sign-in
  after the reboot) and tokens (the no-auth sim has no admin until `ensureAdmin`). Under `--auth`
  its tests skip themselves (`hub` fixture). Add a spec there when its no-auth behaviour differs.
- A mock run and a hub-sim run can go at once: Vite on :5181 (mock) / :5182 (sim), output in
  `test-results/<mock|sim>` and `playwright-report/<mock|sim>`, Vite cache
  `node_modules/.vite-e2e-<target>` (`HUBBO_VITE_CACHE_DIR`). Skipped tests cost ~1–2 s a run.
  Gotcha: two `docker compose run` of the same service starting at the same moment lose the exit
  code (254, compose v2.21, ~20%), so parallel branches must not start the same service together.
- hub-sim is still slow (`workers: 1`, real-time polls, HA/BLE waits). Backlog: a hub-sim port per
  worker.
- Flake seen once in the concurrent gate (hub-sim, activity.spec "closing the duplicate dialog
  while it saves"): toast shown, copy missing from the list. Not reproduced in ~75 runs under load.
  Ruled out: the dialog's close event (it fires right after cancel), hub-sim (commits before the
  201), browser caching, proxy errors. Fixed the one hole that fits: `invalidate()` during an
  in-flight fetch reused that fetch (older than the write) and nothing refetched; now it fetches
  again after (api/hooks.test.ts). The spec checks the URL before the copy, so a recurrence says
  which way it failed.
  Seen again 2026-11-13 (ha-domains gate, hub-sim): the other way, it navigated to the copy after
  Escape (so the close's `dupRequest` bump came before the submit's). Passed on rerun; not chased.
- Under `--auth`, first-run tests factory-reset hub-sim themselves (hub.ts `factoryResetSim`), so
  setup.spec runs whole in a gate pass. hub-sim starts paired, so the resume test opens
  `#/setup/remote` by URL there.
- Blocked seeds: `bin/hubbo sim write-record --data DIR --name config --file
  sim/fixtures/config.newer-schema.json`, then `serve` without `--reset`, then `--project seeded
  --no-deps`.
- **hub-sim quirks**:
  - DELETE /activities/<id> needs `?revision=N`.
  - Registering needs the entity's details first (`registerHa`); poll discovery to `done` first.
  - Bluetooth: dev_apple_tv starts bonded and connected (after /sim/reset too).
  - Sim time: HA reconnects and remote event ages only move with `/sim/advance`. Specs advance
    1 s before a press so a new event is younger than the last.
  - `/sim/reset` restores entities, BLE and the radio, and releases held buttons. Specs tap.
  - A re-link is checked against the activities that use the device: re-link a fresh device.
- Auth: no-auth sim = AUTH DISABLED; `--auth` is the production authenticator. Login specs skip
  without auth.
  - hub-sim's firmware version doesn't change after an update (so there the UI says "didn't
    take") and its sessions survive the simulated reboot; the reboot can fall between two 1 s
    polls. firmware.spec allows for all three.
  - hub-sim answers no 501s now: `unavailable.spec` (stubbed 501s) runs on the mock only.

## Key decisions

- Secrets: nothing secret in browser storage (no CSRF token any more); the HA token is write-only. e2e checks with `expectNotStoredInBrowser`.
- Backups are an opaque blob; restore sends the file exactly as chosen.
- Never round-trip `ha_capabilities` (422 unknown_field): PUTs send name/kind/provider/revision.
- HA TLS: "Verify certificate" (`verify_tls`, https only) is real; no CA field.
- A stale device's commands are a 200 `failed` / `entity_not_found`.
- Discovery is async everywhere: wait for `done` before registering or re-linking. `truncated` > 0
  shows a count, no action.
- Remote address rules live once, in `lib/remote.ts addressProblem` (UI and mock).
- Don't restyle existing pages (the redesign is deferred). Hobby scale.

## Open items

- The polish rounds aren't committed; `/simplify` done. Gate 2026-11-10 after the review fixes:
  green (unit 82; mock 102 passed; `--sim --auth` 64; `noauth` 4).
- Accepted by the lead, leave as is: Recent errors show the raw message (Status is the debug
  page); the 3 s /integrations poll; the header can lag an HA save by up to 2 s.
- Skipped in polish /simplify: one route table shared by App and the nav; a RoutedTabs helper for
  Status and Settings; moving `LIMITS` out of the entry chunk; `useStatus` by Status tab (changes
  the uptime refresh); one `inSetup` banner prop (changes copy for a non-admin); `Section`
  regions to replace the e2e `exact: true` locators (changes the a11y tree); an `ErrorState`
  overrides prop for the per-component error-code copy; deriving the post-restore token notice
  from the preview's `requires` (behavior); dropping `export` from file-local helpers.
- Skipped in Phase 5 /simplify: drive the firmware poll through `useQuery` pollMs (drops the run
  ref and eslint-disables); a shared ProgressBar (PairRemote has the same markup); one mock clock
  for every timer; hub-sim reporting the new version and dropping
  sessions after a simulated reboot (core/API ask) would remove the `SIM_URL` branches.
- Flaky once each, passed on rerun: `activity.spec:163` (mock), `watch-apple-tv` (hub-sim).
- Board check of the firmware flow is pending (the real reboot: network errors, then a 401).
- Typed params can't be previewed before registering (candidates carry only command ids).
- Closing the Duplicate dialog mid-save doesn't navigate to the copy.
- Possible later trims (skipped in /simplify):
  - Keep the wizard step in the URL always (drops the resume effect and the e2e `SIM_URL` goto).
  - Load entity details through `useQuery` instead of the `loadId` counter, or one helper that
    owns details → mutate → error mapping (or the hub loading details itself: an API ask).
  - Have /status report the settings revision the HA client runs, instead of the 2 s grace (core ask).
  - A `useShowErrorsOnSubmit` hook for the five forms that repeat the `attempted` pattern.
  - One fake-HA entity set for mock and hub-sim (then drop the mock rename control and the e2e
    target tables); have the mock's scan wait for `radio/found` like hub-sim.
  - `MockRequest` raw-only; a board target in `hub.ts`; derive the remote's `HIT` from the scale.
