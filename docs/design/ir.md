# IR: sending infrared codes from device profiles

The hub sends IR through an emitter wired to the board, so TVs, receivers and anything else with an
IR remote are ordinary devices: their commands bind to buttons, startup and shutdown like a
Bluetooth device's.

## Rules

- **Codes come from a device profile**: found by searching the catalog in Add device
  ([catalog-hosting.md](catalog-hosting.md)) or imported as a profile file. The profile holds the
  Pronto codes, converted from the Harmony archive for catalog devices.
- **The pin is fixed: GPIO 1** (header pin 25). There's no setting. `firmware/README.md` documents
  the wiring.
- **Send only.** No learning; that needs a receiver.

## Hardware

Any active-high emitter module with its own driver transistor works, for example Adafruit's High
Power IR LED Emitter (two 940 nm LEDs behind an N-FET, 3–5 V logic).
- Signal to GPIO 1, V+ to VSYS (5 V), GND to GND.
- On VSYS each pulse draws about 400 mA; that's why it isn't on 3V3.
- A bare IR LED needs a transistor; a GPIO can't drive it.

## What an IR device is

- **A profile that's been added**, exactly as in [device-profiles.md](device-profiles.md), with
  `platform: ir`. It keeps every command, `power`, `timing` and `inputs`, so the wizard fills
  buttons, inputs and power steps from it as it does for Bluetooth.
- **Integration:** the built-in `ir`, next to `ble`. It's always there; it reports whether the
  transmitter is ready.
- **No pairing, no connection state.** Adding is the whole setup. IR commands never fail for being
  "not connected"; nothing can tell whether the device saw them.
- **No device limit** beyond the config's size cap (`kMaxConfigFileBytes`, 512 KB). An IR profile
  is about 16 KB of JSON (30 KB at p95, 40 KB at most), so a home's worth fits. A device that
  would push the config over the cap is refused with the usual error.
- Pronto is parsed into frames once, when a device is added or the config loads, never on a press.

## Commands

The `remote` group (`press`, `press_down`, `press_up` with a profile command name) works for IR
devices, as defined by the profile spec's IR section:
- **A tap** sends `timing.ir_repeats` frames (at least one): the once section, then repeat frames.
- **A hold** (press_down … press_up) sends the once section, then repeat frames until the release.
  The remote's release timeout ends a hold whose up packet was lost.
- **`hold_ms`** on a command, or `press {command, hold_ms}`, holds for that long.
- **Toggle codes:** with `pronto_toggle`, the hub keeps one toggle bit per device (RAM only, starts
  at 0), flips it on each new press and sends the matching code.
- **`key_delay_ms`** spaces consecutive commands to the same device, as for Bluetooth.
- The `keys` and `custom` groups are Bluetooth only.

One transmitter means one IR code at a time. A new IR command waits for the frame in flight to end;
a hold on one device ends when a command for another IR device starts.

On the board the transmitter uses the ESP32's RMT with a carrier from the Pronto frequency, on its
own task, so the RF loop's timing is unchanged. hub-sim has a fake transmitter that records what was
sent (`GET /sim/ir`).

## Importing a profile

- **Add device → Import a profile file.** Pick a `.json` file; the hub validates it as a complete
  profile, shows its manufacturer and model, and you name it. The same path takes any platform; a
  Bluetooth profile then pairs as usual.
- **Validation** follows the spec: field ranges, command names, Pronto well-formed (header length,
  no zero durations, a nonzero frequency word: the catalog has codes as low as 9 kHz). Up to the
  spec's 512 commands; the size cap is what bounds IR. An invalid file is refused with the field
  that's wrong.
- **Where files come from:** each device page on the [catalog](https://hubbo.tv/devices/) offers
  its complete profile (the device joined with its codeset).
