#include "ha_task.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/queue.h>
#include <freertos/task.h>

namespace hubbo::fw {

namespace {
constexpr UBaseType_t kQueueDepth = 8;
using Job = std::function<void()>;
}  // namespace

bool HaTask::start(Service service) {
    if (queue_ != nullptr) {
        return true;
    }
    QueueHandle_t queue = xQueueCreate(kQueueDepth, sizeof(Job *));
    if (queue == nullptr) {
        return false;
    }
    service_ = std::move(service);
    queue_ = queue;
    if (xTaskCreatePinnedToCoreWithCaps(&HaTask::run, "ha", kStackBytes, this, kPriority, nullptr, kCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(queue);
        queue_ = nullptr;
        return false;
    }
    return true;
}

bool HaTask::post(std::function<void()> job) {
    if (queue_ == nullptr) {
        return false;
    }
    auto *owned = new Job(std::move(job));
    if (xQueueSend(static_cast<QueueHandle_t>(queue_), &owned, 0) != pdTRUE) {
        delete owned;
        return false;
    }
    return true;
}

void HaTask::run(void *arg) {
    auto *self = static_cast<HaTask *>(arg);
    auto queue = static_cast<QueueHandle_t>(self->queue_);
    TickType_t wait = 0;
    for (;;) {
        Job *job = nullptr;
        while (xQueueReceive(queue, &job, wait) == pdTRUE) {
            (*job)();
            delete job;
            wait = 0;
        }
        const int64_t before = esp_timer_get_time();
        const bool busy = self->service_();
        // Nothing waited and nothing pending: sleep on the queue, not spin core 0.
        wait = !busy && esp_timer_get_time() - before < 1000 ? pdMS_TO_TICKS(kIdleWaitMs) : 0;
    }
}

}  // namespace hubbo::fw
