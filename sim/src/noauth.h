#pragma once

#include "host_guard.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/policy.h"
#include "hubbo/http/route.h"

// SIMULATOR_NO_AUTH: every request carries the auth module's simulator
// principal (a simulated admin). Compiled only when the host-only build
// defines HUBBO_SIMULATOR_NO_AUTH (set by sim/CMakeLists.txt for hub-sim;
// never by the firmware build). Only fake adapters exist in hub-sim, so this
// can't reach real devices.
#if !defined(HUBBO_SIMULATOR_NO_AUTH)
#error "noauth.h requires the host-only HUBBO_SIMULATOR_NO_AUTH build flag"
#endif

namespace hubbo::sim {

constexpr const char *kNoAuthBanner = "AUTH DISABLED \xE2\x80\x94 SIMULATOR";

// Every request is the simulator principal; everything else about the
// request context (whether an admin exists yet) is the real state, so setup
// and token creation behave as on the hub.
class SimulatorNoAuth : public http::RequestAuthenticator {
public:
    explicit SimulatorNoAuth(const auth::Credentials &credentials) : credentials_(credentials) {}

    auth::RequestContext authenticate(const http::Request &) override {
        auth::RequestContext context;
        context.principal = auth::Principal::simulator();
        context.adminExists = credentials_.adminExists();
        return context;
    }

private:
    const auth::Credentials &credentials_;
};

}  // namespace hubbo::sim
