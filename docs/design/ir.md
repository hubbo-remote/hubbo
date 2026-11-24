# IR: sending infrared codes from device profiles

Started 2026-11-24 (user). The hub sends IR through an emitter wired to the board, so TVs,
receivers and anything else with an IR remote become ordinary devices: their commands bind to
buttons, startup and shutdown like a Bluetooth device's.

## Decisions (user, 2026-11-24)

- **Codes come from an imported profile file** in this phase. Add device takes a profile JSON (an
  IR profile from the catalog, which already holds the converted Harmony codes). Hosting the
  catalog and searching it from the web admin is a later phase.
- **The pin is fixed: GPIO 1** (header pin 25). There's no setting. The README documents the
  wiring.
- **Send only.** No learning; that needs a receiver and stays in the backlog.

## Hardware

Tested with Adafruit's High Power IR LED Emitter (product 5639): two 940 nm LEDs behind an N-FET,
an active-high signal, 3–5 V logic, and a red LED that lights while it sends.
- Signal to GPIO 1, V+ to VSYS (pin 39, shared with the radio adapter), GND to GND.
- On VSYS each pulse draws about 400 mA; that's why it isn't on 3V3.
- Any emitter that's active high works the same way. A bare IR LED needs a transistor; a GPIO
  can't drive it.

## What an IR device is

- **A profile that's been added**, exactly as in `device-profiles.md`, with `platform: ir`. It
  keeps every command, `power`, `timing` and `inputs`, so the wizard fills buttons, inputs and
  power steps from it as it already does for Bluetooth.
- **Integration:** a new built-in `ir`, next to `ble`. It's always there; it reports whether the
  transmitter is ready.
- **No pairing, no connection state.** Adding is the whole setup. IR commands never fail for being
  "not connected"; nothing can tell whether the device saw them.
- **No device limit** beyond the config's size cap (`kMaxConfigFileBytes`, 512 KB). An IR profile
  is about 16 KB of JSON (30 KB at p95, 40 KB at most), so a home's worth fits. A device that
  would push the config over the cap is refused with the usual error.

## Commands

The `remote` group (`press`, `press_down`, `press_up` with a profile command name) works for IR
devices, as defined by the spec's IR section (`~/src/hubbo-profiles/docs/spec.md`):
- **A tap** sends `timing.ir_repeats` frames (at least one): the once section, then repeat frames.
- **A hold** (press_down … press_up) sends the once section, then repeat frames until the release.
  The remote's existing release timeout ends a hold whose up packet was lost.
- **`hold_ms`** on a command, or `press {command, hold_ms}`, holds for that long.
- **Toggle codes:** with `pronto_toggle`, the hub keeps one toggle bit per device (RAM only, starts
  at 0), flips it on each new press and sends the matching code.
- **`key_delay_ms`** spaces consecutive commands to the same device, as for Bluetooth.
- The `keys` and `custom` groups are Bluetooth only.

One transmitter means one IR code at a time. A new IR command waits for the frame in flight to end;
a hold on one device ends when a command for another IR device starts.

## Importing a profile

- **Add device → Import a profile file.** Pick a `.json` file; the hub validates it as a complete
  profile, shows its manufacturer and model, and you name it. The same path takes any platform
  (an IR or a Bluetooth profile); a Bluetooth one then pairs as usual.
- **Validation** follows the spec: field ranges, command names, Pronto well-formed (header length,
  no zero durations, a nonzero frequency word: the catalog has codes as low as 9 kHz). Up to
  the spec's 512 commands (the hub's old 128 cap was for Bluetooth; the size cap is what bounds
  IR). An invalid file is refused with the field that's wrong.
- **Where files come from:** each device page on the catalog site offers its complete profile
  ("Download profile": the device joined with its codeset). Until the site is hosted, that's the
  local site build, or `tools/bin/profile <id>` in hubbo-profiles.

## Owners

- **firmware:** the IR transmitter on GPIO 1 (ESP32 RMT with a carrier from the Pronto frequency),
  sending on its own path so the RF loop's timing is unchanged; holds that repeat until release;
  the `ir` integration's ready state; the README wiring section. First, a spike that sends one
  Pronto code from the console and reports timing accuracy.
- **core:** the `ir` integration and device platform; parsing Pronto into frames once, when a
  device is added or the config loads; the transmitter port and the action semantics above
  (repeats, holds, toggles, key delay, one-at-a-time); hub-sim's fake transmitter that records
  what was sent (`GET /sim/ir`), with tests.
- **api:** adding a device from an uploaded profile (`POST /devices` with the whole profile), its
  validation errors, IR devices in `DeviceDetail`, the `ir` integration in status; spec,
  fixtures, contract, docs/agent-api.md.
- **frontend:** Import a profile file in Add device; IR devices in the device list and pickers
  (no pairing card); mock and e2e (import an IR profile, bind a command, run it on hub-sim).
- **spec:** check the spec says everything the hub needs for IR, and that the site's per-device
  download is the complete profile. Provide a few small IR test profiles for fixtures.
- **lead:** this doc, ARCHITECTURE.md, the board test.

## Board test (dev board, .196)

1. Send a code from the console; the emitter's red LED blinks and the target device reacts.
2. Import a real IR profile for a device in the room; bind power, volume and the d-pad.
3. Taps, holds (volume ramps while held, stops on release), toggle codes, `ir_repeats`.
4. A startup with IR power and input steps.
5. The loop gap and missed-press rate stay as they are while IR sends.
