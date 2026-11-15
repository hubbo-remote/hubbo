# Smart buttons

Approved by the user on 2026-11-14 and simplified on 2026-11-15, after the board test. The remote's
Light 1, Light 2, Switch 1 and Switch 2 buttons can each be given a device. The hub decides what the
button and +/- do from that device's type. +/- control whichever of the four was pressed last. This is
how the Harmony remote's Home Control buttons worked.

## What the user decided

- **Four slots:** `light_1`, `light_2`, `switch_1`, `switch_2`. Each takes a light, fan, cover or
  switch, or no device.
- **Pick a device, and that's all.** Nothing else is configurable: the actions come from the table
  below, built only from commands the device offers (what HA provides).
- **On the Global page.** There's no separate Smart buttons page. Clicking Light 1 on the Global
  remote shows a device picker. With a device, the panel shows what the button and +/- will do,
  read-only. With no device, Light 1 is an ordinary button, bound like any other. Picking a device
  clears that button's global bindings when you save (a second request after the slots), so None
  starts empty.
- **Activities show them as inherited.** A slot button with a device, and + / − when any slot has
  one, show what the smart button does, tagged like an inherited Global binding, with the usual rows
  below to override it in that activity.
- **+ and − on Global belong to the smart buttons** once any slot has a device: they can't be bound
  there, and the panel explains what they do instead. Picking the first device clears Global's + and −
  bindings, as picking a device clears that button's own. With no devices, + and − are ordinary
  buttons. A slot with no +/- actions (empty, or a switch) leaves + and − doing nothing.
- **Activities override, like any button.** People without smart devices can use these buttons for
  anything: leave the slot empty and bind it.
- **Quick +/- taps merge.** Taps on a + or − whose action has a `step` merge into one call (below).
- **After a reboot, Light 1 is selected.**

## The config

```json
"smart_buttons": {"revision": 4,
                  "slots": {"light_1": "dev_lamp", "light_2": null, "switch_1": "dev_fan", "switch_2": null}}
```

- All four slots are always present. A fresh hub has four nulls.
- A saved config or backup without `smart_buttons` loads with four empty slots (the one approved
  exception to the no-compat rule, for hubs in daily use).
- A slot's device must exist. Deleting a device a slot uses is refused (409 `device_in_use`, path
  `/smart_buttons/slots/<slot>`), like other references.
- `PUT /smart-buttons` takes the same object; `revision` is the base, and a stale one is 409.
- `GET /smart-buttons` returns it, plus each slot's derived actions, read-only, for the web to show:
  `"actions": {"light_1": {"button": {"tap": [...], "hold": [...], "release": [...]}, "plus": {...},
  "minus": {...}}, ...}`.

## The actions, by type

Worked out by the hub when it compiles the config, from the device's `kind` and the commands it
offers. Anything the device can't do is left out, or uses the fallback shown. Another kind, or a
non-HA device, gets no actions.

| Type | Press | Long press | + | − |
|---|---|---|---|---|
| Light | Turn on | Turn off | Change brightness +10% | −10% |
| Fan | Turn on | Turn off | Speed up | Slow down |
| Cover | Open | Close | Change position +10; long press: Open, Stop on release | −10; long press: Close, Stop on release |
| Switch | Turn on | Turn off | none | none |

- **Light** steps are `light.brightness_step` `step` ±26 (10% of 255).
- **Cover with no position:** + is Open, − is Close, with no long press.
- **Cover** uses `cover.position_step` (below).

## How a press resolves

Per button, the first of these wins:

1. The active activity, if it binds any trigger of that button.
2. The slot, if it has actions: its own button for Light 1/2 and Switch 1/2, or the **selected**
   slot's +/- for + and −.
3. The global bindings.

- **Selection:** Down on a slot's button selects it, even an empty one, unless the active activity
  binds that button. It lives in RAM and starts at `light_1` on boot.
- Slot actions run with the sequence origin `smart_button`.
- **Release** fires only on Up after Hold, including one synthesized after a lost packet. It's what
  stops a moving cover. The slot a press belongs to is fixed on Down.
- No repeat: a hold fires once.

## Merging quick +/- taps

A Tap on + or − merges when the selected slot's tap action has an int `step` param. It isn't sent; it
adds one to a pending count and starts a 300 ms timer. A Down of the same button stops the timer until
its Up. When the timer runs out, one call goes out with `step` × count, clamped to the param's range.

- The pending taps are sent at once on a tap the other way, a slot change, an activity change or off,
  and a Hold on + or −.
- Everything else is instant: other buttons, a slot's own button, and +/- without a `step` (a fan).

## The command: Change position

`cover.position_step` ("Change position"), with `step` (−100..100, %), on any cover that supports
`SET_POSITION`. The adapter reads `current_position`, adds `step`, clamps to 0..100, and sends
`cover.set_cover_position` with `{position}`. With no `current_position` it fails with
`state_unavailable`.

## Owners

- **core:** the config (`slots` as device ids, decode, encode, backup and restore); deriving each
  slot's actions at compile, from the table (moved from the API's defaults route); the engine
  (selection, resolution, release, the tap merge) with repeat removed; the sim showcase.
- **api:** `GET`/`PUT /smart-buttons` in the new shape, with the derived actions on GET;
  `GET /smart-buttons/defaults` removed; the spec, fixtures and contract; docs/agent-api.md.
- **frontend:** the Global page's panel for Light 1/2 and Switch 1/2 (a device picker, and the
  read-only actions); + and − noting that they follow the selected smart button; the Smart buttons
  choice and editor removed; the mock and e2e.
- **lead:** this doc, and moving .196's config to the new shape.
