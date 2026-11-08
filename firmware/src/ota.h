#pragma once

// ArduinoOTA, compiled in only when HUBBO_OTA_PASSWORD was set in the
// environment at build time (same pattern as Harmonizer's OTA_PASSWORD).
// An empty password means OTA is off. The password is never printed.
//
// Later phases replace this with signed, authenticated updates over the API.

namespace hubbo::fw::ota {

bool enabled();

// Call once the network has an address. onStart runs on the main loop just
// before the flash write begins (release held keys there).
void begin(const char *hostname, void (*onStart)());

void loop();

}  // namespace hubbo::fw::ota
