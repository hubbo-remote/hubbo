#include "ota.h"
#include "console.h"
#include "update_lock.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include <atomic>

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

// The RF loop never polls the OTA socket while idle: every lwIP socket call
// takes lwIP's global SYS_ARCH_PROTECT mutex, which the network stack and the
// HA task hold on core 0, so under heavy traffic (an HA discovery) a poll
// stalled the loop for up to ~6 ms. A core-0 watcher blocks in select() on
// ArduinoOTA's socket instead and flags the loop, which then runs handle()
// for a while (the auth reply and the transfer follow the first packet).
constexpr uint16_t kPort = 3232;  // ArduinoOTA's default, set explicitly
constexpr uint32_t kActiveMs = 10000;
constexpr uint32_t kWatcherStackBytes = 2560;
std::atomic<bool> packetWaiting{false};
TaskHandle_t watcher = nullptr;
int otaSocket = -1;
bool pollAlways = false;   // no socket found: poll every loop
bool active = false;        // a packet came in the last kActiveMs
uint32_t lastPacketMs = 0;

int findOtaSocket() {
    for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
        int type = 0;
        socklen_t typeLen = sizeof(type);
        sockaddr_storage addr = {};
        socklen_t addrLen = sizeof(addr);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &typeLen) != 0 || type != SOCK_DGRAM ||
            getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addrLen) != 0) {
            continue;
        }
        const uint16_t port = addr.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6 &>(addr).sin6_port
                                                         : reinterpret_cast<sockaddr_in &>(addr).sin_port;
        if (ntohs(port) == kPort) {
            return fd;
        }
    }
    return -1;
}

void watch(void *) {
    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(otaSocket, &readable);
        if (select(otaSocket + 1, &readable, nullptr, nullptr, nullptr) > 0) {
            if (!update_lock::claimable(update_lock::Owner::Arduino)) {
                uint8_t drop[16];  // another holder or a reboot: no answer to espota
                recv(otaSocket, drop, sizeof(drop), MSG_DONTWAIT);
                continue;
            }
            packetWaiting.store(true);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // until the loop has read it
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// ArduinoOTA's whole transfer runs inside one handle() call, so holding the
// flag across the call covers it. Between calls (waiting for the auth reply)
// it's free.
void handleIfOwner() {
    if (!update_lock::claim(update_lock::Owner::Arduino)) {
        return;  // someone else holds it, or a reboot is pending
    }
    ArduinoOTA.handle();
    update_lock::release(update_lock::Owner::Arduino);  // after a written image, the pending reboot refuses new claims
}
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
    ArduinoOTA.setPort(kPort);
    ArduinoOTA.setMdnsEnabled(false);      // HubName runs mDNS for the whole hub
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
    otaSocket = findOtaSocket();
    pollAlways = otaSocket < 0 ||
                 xTaskCreatePinnedToCoreWithCaps(&watch, "ota_watch", kWatcherStackBytes, nullptr, 1, &watcher, 0,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS;
    if (pollAlways) {
        con.println("ota: no socket watcher; polling from the loop");
    }
    con.printf("ota: enabled as %s.local\n", hostname);
}

void loop() {
    if (!started) {
        return;
    }
    const uint32_t nowMs = millis();
    if (packetWaiting.exchange(false)) {
        active = true;
        lastPacketMs = nowMs;
        handleIfOwner();
        xTaskNotifyGive(watcher);
        return;
    }
    active = active && nowMs - lastPacketMs < kActiveMs;
    if (pollAlways || active) {
        handleIfOwner();
    }
}

}  // namespace hubbo::fw::ota
