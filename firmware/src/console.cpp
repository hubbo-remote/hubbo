#include "console.h"

#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace hubbo::fw {

Console con;

namespace {

constexpr size_t kSerialTxBuffer = 4096;

#ifdef HUBBO_DEV_CONSOLE
// Written by any task that prints, read by httpd: a mutex, held only for the
// copy (never across Serial I/O).
char ring[Console::kRecentBytes];
size_t ringHead = 0;
bool ringFull = false;
SemaphoreHandle_t ringLock = nullptr;
#endif

}  // namespace

void Console::begin(unsigned long baud) {
    Serial.setTxBufferSize(kSerialTxBuffer);  // must precede begin()
    Serial.begin(baud);
    Serial.setTxTimeoutMs(0);  // never wait for the USB host
#ifdef HUBBO_DEV_CONSOLE
    ringLock = xSemaphoreCreateMutex();
#endif
}

size_t Console::write(const uint8_t *buffer, size_t size) {
    Serial.write(buffer, size);  // non-blocking; may drop
#ifdef HUBBO_DEV_CONSOLE
    const size_t kept = std::min(size, kRecentBytes);  // only the tail fits
    const uint8_t *tail = buffer + (size - kept);
    if (ringLock != nullptr && xSemaphoreTake(ringLock, portMAX_DELAY) == pdTRUE) {
        const size_t first = std::min(kept, kRecentBytes - ringHead);
        memcpy(ring + ringHead, tail, first);
        memcpy(ring, tail + first, kept - first);
        ringFull = ringFull || ringHead + kept >= kRecentBytes;
        ringHead = (ringHead + kept) % kRecentBytes;
        xSemaphoreGive(ringLock);
    }
#endif
    return size;
}

#ifdef HUBBO_DEV_CONSOLE
std::string Console::recent() const {
    std::string out;
    out.reserve(kRecentBytes);  // allocate before taking the lock
    if (ringLock != nullptr && xSemaphoreTake(ringLock, portMAX_DELAY) == pdTRUE) {
        if (ringFull) {
            out.assign(ring + ringHead, kRecentBytes - ringHead);
        }
        out.append(ring, ringHead);
        xSemaphoreGive(ringLock);
    }
    return out;
}
#endif

}  // namespace hubbo::fw
