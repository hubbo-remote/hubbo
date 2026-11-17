#include "integration_task.h"

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

bool IntegrationTask::start(const char *name, Service service, uint32_t idleWaitMs) {
    if (queue_ != nullptr) {
        return true;
    }
    QueueHandle_t queue = xQueueCreate(kQueueDepth, sizeof(Job *));
    if (queue == nullptr) {
        return false;
    }
    service_ = std::move(service);
    idleWaitMs_ = idleWaitMs;
    queue_ = queue;
    if (xTaskCreatePinnedToCoreWithCaps(&IntegrationTask::run, name, kStackBytes, this, kPriority, nullptr, kCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        vQueueDelete(queue);
        queue_ = nullptr;
        return false;
    }
    return true;
}

bool IntegrationTask::post(std::function<void()> job) {
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

void IntegrationTask::run(void *arg) {
    auto *self = static_cast<IntegrationTask *>(arg);
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
        wait = !busy && esp_timer_get_time() - before < 1000 ? pdMS_TO_TICKS(self->idleWaitMs_) : 0;
    }
}

}  // namespace hubbo::fw
