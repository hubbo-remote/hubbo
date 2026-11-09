#pragma once

// api::RemotePort over RemoteRadio. httpd calls it; RemoteRadio is loop-only,
// so status() answers from a copy the loop refreshes (publish()), and
// requests go through one mailbox slot the loop takes (poll()). Neither side
// waits for the other: a spinlock guards only the copies.

#include <freertos/FreeRTOS.h>

#include "hubbo/api/remote_port.h"
#include "remote_radio.h"

namespace hubbo::fw {

class RemoteRuntime : public hubbo::api::RemotePort {
public:
    explicit RemoteRuntime(RemoteRadio &radio) : radio_(radio) {}

    // httpd.
    hubbo::api::RemoteRadioStatus status() const override;
    hubbo::api::RemoteResult setEndpoint(const hubbo::api::RemoteEndpoint &endpoint) override;
    hubbo::api::RemoteResult startScan(uint32_t timeoutMs) override;
    void stopScan() override;

    // Loop: run a requested change, and refresh the status copy.
    void poll(uint32_t nowMs);
    void publish(uint32_t nowMs);

private:
    enum class Request : uint8_t { None, SetEndpoint, StartScan, StopScan };

    hubbo::api::RemoteResult post(Request request, const RadioEndpoint &endpoint, uint32_t timeoutMs);

    RemoteRadio &radio_;
    mutable portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
    hubbo::api::RemoteRadioStatus status_;  // guarded
    Request request_ = Request::None;        // guarded: the mailbox slot
    RadioEndpoint requestEndpoint_;
    uint32_t requestTimeoutMs_ = 0;
};

}  // namespace hubbo::fw
