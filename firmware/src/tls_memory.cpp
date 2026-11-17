#include "tls_memory.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/platform.h>

#include <cstdint>

namespace hubbo::fw::tls {

namespace {

constexpr int64_t kSliceUs = 20000;

// Per task (ESP-IDF keeps thread_local on each task's stack).
thread_local bool yielding = false;
thread_local int64_t sliceStartUs = 0;

void *calloc(size_t count, size_t size) {
    yieldIfDue();
    return heap_caps_calloc_prefer(count, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

}  // namespace

void useExternalMemory() {
    mbedtls_platform_set_calloc_free(calloc, heap_caps_free);
}

void yieldIfDue() {
    if (!yielding) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (now - sliceStartUs >= kSliceUs) {
        vTaskDelay(1);
        sliceStartUs = esp_timer_get_time();
    }
}

YieldScope::YieldScope() {
    sliceStartUs = esp_timer_get_time();
    yielding = true;
}

YieldScope::~YieldScope() {
    yielding = false;
}

}  // namespace hubbo::fw::tls
