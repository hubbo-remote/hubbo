#include "ota.h"

#include <Arduino.h>
#include <ArduinoOTA.h>

#ifndef HUBBO_OTA_PASSWORD
#define HUBBO_OTA_PASSWORD ""
#endif

namespace hubbo::fw::ota {

namespace {
bool started = false;
void (*startHook)() = nullptr;
}  // namespace

bool enabled() {
    return sizeof(HUBBO_OTA_PASSWORD) > 1;
}

void begin(const char *hostname, void (*onStart)()) {
    if (!enabled() || started) {
        return;
    }
    startHook = onStart;
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setPassword(HUBBO_OTA_PASSWORD);
    ArduinoOTA.onStart([] {
        Serial.println("ota: update starting");
        if (startHook != nullptr) {
            startHook();
        }
    });
    ArduinoOTA.onEnd([] { Serial.println("ota: update written, rebooting"); });
    ArduinoOTA.onError([](ota_error_t error) {
        // Only the error class; never anything credential-related.
        Serial.printf("ota: error %u\n", static_cast<unsigned>(error));
    });
    ArduinoOTA.begin();
    started = true;
    Serial.printf("ota: enabled as %s.local\n", hostname);
}

void loop() {
    if (started) {
        ArduinoOTA.handle();
    }
}

}  // namespace hubbo::fw::ota
