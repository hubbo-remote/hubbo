#pragma once

// An integration task: pinned to core 0 at priority 4
// (below httpd at 5: a TLS handshake is seconds of CPU and must not stall
// the API; W5500 RX is at 15 and lwIP at 18), with an 8 KB
// stack in PSRAM (it never touches flash: secrets reach it from httpd/boot).
// Two run on it: "ha" owns the Home Assistant WebSocket connection, and
// "mqtt" the MQTT bridge (mqtt_transport.h). The RF loop on core 1 never
// waits on either.
//
// Each pass: run posted jobs, then the service callback (transport.poll(),
// client.poll(), adapter.service()). The service's socket wait is the pass's
// sleep. A pass that didn't wait and has no pending work (e.g. no
// connection) sleeps kIdleWaitMs on the job queue, so IDLE0 gets the core
// and a posted job still runs at once.

#include <cstdint>
#include <functional>

namespace hubbo::fw {

class IntegrationTask {
public:
    static constexpr int kCore = 0;
    static constexpr unsigned kPriority = 4;
    static constexpr uint32_t kStackBytes = 8192;
    static constexpr uint32_t kIdleWaitMs = 10;    // a pass that neither waited nor has work
    static constexpr uint32_t kSocketWaitMs = 50;  // the service's transport.poll() wait

    // Returns true while there's work the next pass should do at once. It
    // reads the clock itself: its own waits make a time taken before it stale.
    using Service = std::function<bool()>;
    // `idleWaitMs`: how long an idle pass sleeps on the job queue (a service
    // with nothing due for seconds can sleep longer; a posted job still runs
    // at once).
    bool start(const char *name, Service service, uint32_t idleWaitMs = kIdleWaitMs);

    // Any task: run `job` once on this task. False if not running or the
    // queue is full.
    bool post(std::function<void()> job);

private:
    static void run(void *self);

    Service service_;
    uint32_t idleWaitMs_ = kIdleWaitMs;
    void *queue_ = nullptr;  // QueueHandle_t of std::function<void()> *
};

}  // namespace hubbo::fw
