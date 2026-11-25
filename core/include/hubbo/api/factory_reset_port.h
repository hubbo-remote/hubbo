#pragma once

#include <cstdint>
#include <functional>

// The real factory reset, POST /admin/factory-reset (docs/security.md,
// "Factory reset"). Not POST /admin/reset,
// which resets only the configuration.
//
// The route answers 202, then the platform erases everything and restarts
// into first-run setup: the login, API tokens, the HA token, BLE bonds, the
// remote endpoint (the board: NVS) and the config filesystem. The board
// reuses the wipe code (NVS erase, then the filesystem format at the next
// mount), off the RF task, releasing held keys first like any reboot.
namespace hubbo::api {

class FactoryResetPort {
public:
    virtual ~FactoryResetPort() = default;

    // Called on the HTTP task; returns quickly. True: the reset will happen
    // after this response is sent (asking again while one is pending is also
    // true). False: the hub can't do it now (the route answers 503). The
    // route answers 409 itself while a firmware update is running.
    virtual bool requestFactoryReset() = 0;
};

// hub-sim and tests: the "restart" comes kRestartMs of advance() time after
// the request, and calls `erase` (hub-sim: secrets, credentials, config,
// the BLE bond, the remote endpoint, every session). Single task.
class SimulatedFactoryReset : public FactoryResetPort {
public:
    static constexpr uint32_t kRestartMs = 1000;

    bool requestFactoryReset() override;
    void advance(uint32_t elapsedMs);
    bool pending() const { return pending_; }

    bool ready = true;            // false: requestFactoryReset() is refused
    std::function<void()> erase;  // what the simulated restart erases

private:
    bool pending_ = false;
    uint32_t elapsedMs_ = 0;
};

}  // namespace hubbo::api
