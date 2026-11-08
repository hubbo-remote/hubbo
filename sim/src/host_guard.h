#pragma once

// hub-sim is host-only. Its no-auth mode must be impossible on the hub:
// these sources are not part of core/library.json (the firmware never sees
// them), and if they are ever compiled for an ESP32 target the build fails.
#if defined(ARDUINO) || defined(ESP_PLATFORM) || defined(ESP32) || defined(IDF_VER)
#error "sim/ is host-only: the simulator (and its no-auth mode) must never be built into firmware"
#endif
