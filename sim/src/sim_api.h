#pragma once

// hub-sim's HTTP surface:
//   /api/v1/...  the shared API handlers (core/include/hubbo/api), exactly as
//                the firmware serves them, over SimHub (SimRuntime).
//   /sim/...     simulator controls (remote buttons, time, logs, scripts,
//                reset). Never exist on the hub; admin only.
//
// Auth: SIMULATOR_NO_AUTH by default (every request is the simulator
// principal); with `authEnabled` the production api::Authenticator over the
// given secret store (hub-sim persists it under --data).

#include <functional>
#include <memory>
#include <vector>

#include "host_guard.h"
#include "hubbo/api/api.h"
#include "hubbo/api/authenticator.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/rate_limit.h"
#include "hubbo/auth/secret_store.h"
#include "hubbo/auth/session.h"
#include "hubbo/http/route.h"
#include "noauth.h"
#include "runtime.h"
#include "scenario.h"
#include "switchable_storage.h"

namespace hubbo::sim {

class SimApi {
public:
    // `runner` must be pinned (its hub lives as long as this) and boot with
    // `credentials` (they're loaded by the hub's boot, bootHub()), which
    // must outlive this. `storage` (optional) is the medium whose health
    // POST /sim/storage controls.
    SimApi(ScenarioRunner &runner, bool authEnabled, auth::Credentials &credentials,
           SwitchableStorage *storage = nullptr);

    http::RequestAuthenticator &authenticator();
    void handle(http::Request &request, http::Response &response) const { router_.dispatch(request, response); }
    // The route's body limit (router policy), for the transport.
    size_t bodyLimit(http::Method method, std::string_view path) const { return router_.bodyLimit(method, path); }

private:
    class Fn : public http::RouteHandler {
    public:
        using F = std::function<void(const http::Request &, const http::PathParams &, http::Response &)>;
        explicit Fn(F f) : f_(std::move(f)) {}
        void handle(const http::Request &r, const http::PathParams &p, http::Response &out) override { f_(r, p, out); }

    private:
        F f_;
    };

    void host(http::Method method, const char *pattern, Fn::F f);
    std::string simTime() const;

    ScenarioRunner &runner_;
    bool authEnabled_;
    SwitchableStorage *storage_;
    HostRandom random_;
    auth::Credentials &credentials_;
    auth::SessionStore sessions_;
    auth::LoginRateLimiter loginLimiter_;
    SimClock clock_;
    SimRuntime runtime_;
    api::Api api_;
    api::Authenticator productionAuth_;
    SimulatorNoAuth noAuth_;
    http::Router router_;
    std::vector<std::unique_ptr<Fn>> handlers_;
};

}  // namespace hubbo::sim
