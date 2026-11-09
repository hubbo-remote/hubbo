#pragma once

// hub-sim's implementations of what the shared API layer needs from a
// platform (core/include/hubbo/api): a clock, entropy, and a Runtime that
// runs engine requests. Host-only.

#include <string>

#include "host_guard.h"
#include "hub.h"
#include "hubbo/api/api.h"
#include "hubbo/api/authenticator.h"
#include "hubbo/auth/crypto.h"

namespace hubbo::sim {

// std::random_device (getrandom/urandom on Linux).
class HostRandom : public auth::RandomSource {
public:
    bool fill(uint8_t *out, size_t length) override;
};

// Uptime is simulated time; wall clock is the host's.
class SimClock : public api::Clock {
public:
    explicit SimClock(SimHub &hub) : hub_(hub) {}
    uint64_t monotonicMs() const override { return hub_.nowMs(); }
    int64_t unixSeconds() const override;

private:
    SimHub &hub_;
};

// Runs a request by submitting it and advancing simulated time until it is
// done, all on the simulator's single thread (api::Runtime contract: after a
// successful submit, always wait for done()).
class SimRuntime : public api::Runtime {
public:
    // Far beyond any engine bound (action timeout 5 s); tripping it is a bug.
    static constexpr uint32_t kSanityCapMs = 60000;

    explicit SimRuntime(SimHub &hub) : hub_(hub) {}

    bool run(EngineRequest &request) override;
    api::RuntimeSnapshot snapshot() const override;
    const AdapterSet &adapters() const override { return hub_.adapters(); }
    const char *integrationState(std::string_view integrationId) const override;
    const char *deviceAvailability(const DeviceSpec &device) const override;

private:
    SimHub &hub_;
};

}  // namespace hubbo::sim
