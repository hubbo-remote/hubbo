#pragma once

#include <cstdint>
#include <functional>

// POST /admin/restart: reboot the hub after the response is sent. The board
// releases every held output first (like any reboot), off the RF task.
namespace hubbo::api {

class RestartPort {
public:
    virtual ~RestartPort() = default;

    // Called on the HTTP task; returns quickly. True: the restart happens
    // after this response is sent (asking again while one is pending is also
    // true). False: the hub can't restart now (the route answers 503). The
    // route answers 409 itself while a firmware update is running.
    virtual bool requestRestart() = 0;
};

// hub-sim and tests: nothing reboots. The "restart" comes kRestartMs of
// advance() time after the request and calls `restarted` (hub-sim logs it).
// Single task.
class SimulatedRestart : public RestartPort {
public:
    static constexpr uint32_t kRestartMs = 1000;

    bool requestRestart() override;
    void advance(uint32_t elapsedMs);
    // hub-sim's reset: ready, nothing pending; keeps `restarted`.
    void reset();
    bool pending() const { return pending_; }
    int restarts() const { return restarts_; }

    bool ready = true;                // false: requestRestart() is refused
    std::function<void()> restarted;  // optional

private:
    bool pending_ = false;
    uint32_t elapsedMs_ = 0;
    int restarts_ = 0;
};

}  // namespace hubbo::api
