#pragma once

#include <cstdint>

// ArduinoOTA, compiled in only when HUBBO_OTA_PASSWORD was set in the
// environment at build time (same pattern as Harmonizer's OTA_PASSWORD).
// Only its SHA-256 is compiled in (scripts/ota_password_hash.py), never the
// password. Empty means OTA is off. Neither is ever printed.
//
// ArduinoOTA receives and writes the whole image inside handle(), on the
// main loop, for many seconds. So:
//   - the loop task watchdog stays ON. Every progress callback (each chunk
//     received and written) feeds it, so a live transfer keeps it satisfied
//     and a stalled or trickling one trips it and reboots the hub. ArduinoOTA's
//     own receive timeout (kReceiveTimeoutMs, up to 3 retries) and the 3 s
//     connect timeout keep every gap between progress calls under the 5 s
//     watchdog. Progress stops feeding it after kMaxTransferMs (120 s), so a
//     client trickling bytes can't keep the main loop busy forever;
//   - ArduinoOTA does NOT reboot by itself: onWritten runs once the image is
//     written, and the caller restarts the hub its own way (flushing state
//     first, without the main loop waiting on another task).
//
// Idle, the loop doesn't touch the OTA socket (lwIP's lock stalls it under
// load): a core-0 watcher task select()s on it and flags the loop.
//
// One update at a time: ArduinoOTA, a web update (firmware_update.h), a
// factory reset (factory_reset.h) and a restart (restart.h) each claim the
// owner flag below first.
// While anyone else holds it, or a reboot is pending, the loop never runs
// handle() (an espota upload just gets no answer) and the watcher drops OTA
// packets.

namespace hubbo::fw::ota {

// Reboot: a restart or factory reset, held until the restart. Either one
// asking while the other is pending is accepted, and both happen at once.
enum class Owner : uint8_t { Idle, Web, Arduino, Reboot };

// Any task. True if `who` now holds the flag, or already did (a holder's
// repeat claim succeeds even with a reboot pending). Otherwise false once a
// reboot is pending (setRebootPending), so nothing starts writing the slot
// after an update has finished.
bool claim(Owner who);
void release(Owner who);  // no-op unless `who` holds it
bool holds(Owner who);
// Once at boot, before anything claims.
void setRebootPending(bool (*pending)());

bool enabled();

// Call once the network has an address. Both hooks run on the main loop:
// onStart before the transfer begins, onWritten after the image is verified.
void begin(const char *hostname, void (*onStart)(), void (*onWritten)());

void loop();

}  // namespace hubbo::fw::ota
