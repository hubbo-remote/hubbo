#include "ota.h"
#include "console.h"

#include <Arduino.h>
#include <ArduinoOTA.h>

// SHA-256 of the OTA password, hex, from scripts/ota_password_hash.py. The
// password itself is never in the image. Empty: OTA off.
#ifndef HUBBO_OTA_PASSWORD_SHA256
#define HUBBO_OTA_PASSWORD_SHA256 ""
#endif

namespace hubbo::fw::ota {

namespace {
bool started = false;
void (*startHook)() = nullptr;
void (*writtenHook)() = nullptr;
// Per wait for the next chunk; ArduinoOTA retries up to 3 times once data
// has flowed, so a gap between progress calls stays under ~4 s (< the 5 s
// task watchdog).
constexpr int kReceiveTimeoutMs = 1000;
// A whole transfer may take this long. Progress after it no longer feeds the
// watchdog, so a client trickling bytes can't hold the main loop (and the
// remote) hostage: the hub reboots instead. A normal ~2 MB image over
// Ethernet takes seconds.
constexpr uint32_t kMaxTransferMs = 120000;
uint32_t transferStartMs = 0;
}  // namespace

bool enabled() {
    return sizeof(HUBBO_OTA_PASSWORD_SHA256) > 1;
}

void begin(const char *hostname, void (*onStart)(), void (*onWritten)()) {
    if (!enabled() || started) {
        return;
    }
    startHook = onStart;
    writtenHook = onWritten;
    ArduinoOTA.setHostname(hostname);
    ArduinoOTA.setMdnsEnabled(false);      // main runs mDNS for the whole hub
    ArduinoOTA.setRebootOnSuccess(false);  // onWritten -> the caller's reboot path
    ArduinoOTA.setPasswordHash(HUBBO_OTA_PASSWORD_SHA256);
    ArduinoOTA.setTimeout(kReceiveTimeoutMs);
    ArduinoOTA.onStart([] {
        con.println("ota: update starting");
        transferStartMs = millis();
        if (startHook != nullptr) {
            startHook();
        }
    });
    // The transfer runs inside handle(): each chunk proves progress.
    ArduinoOTA.onProgress([](unsigned int, unsigned int) {
        if (millis() - transferStartMs < kMaxTransferMs) {
            feedLoopWDT();
        }
    });
    ArduinoOTA.onEnd([] {
        con.println("ota: update written");
        if (writtenHook != nullptr) {
            writtenHook();
        }
    });
    ArduinoOTA.onError([](ota_error_t error) {
        // Only the error class; never anything credential-related.
        con.printf("ota: error %u\n", static_cast<unsigned>(error));
    });
    ArduinoOTA.begin();
    started = true;
    con.printf("ota: enabled as %s.local\n", hostname);
}

void loop() {
    if (started) {
        ArduinoOTA.handle();
    }
}

}  // namespace hubbo::fw::ota
