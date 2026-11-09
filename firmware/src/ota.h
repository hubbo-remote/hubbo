#pragma once

// ArduinoOTA, compiled in only when HUBBO_OTA_PASSWORD was set in the
// environment at build time (same pattern as Harmonizer's OTA_PASSWORD).
// An empty password means OTA is off. The password is never printed.
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
// Later phases replace this with signed, authenticated updates over the API.

namespace hubbo::fw::ota {

bool enabled();

// Call once the network has an address. Both hooks run on the main loop:
// onStart before the transfer begins, onWritten after the image is verified.
void begin(const char *hostname, void (*onStart)(), void (*onWritten)());

void loop();

}  // namespace hubbo::fw::ota
