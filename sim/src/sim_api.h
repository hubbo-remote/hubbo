#pragma once

// hub-sim's HTTP routes. Two groups:
//
//   /api/v1/...  PROVISIONAL stand-ins (authorized by the auth policy table) for a few API routes (status,
//                activity list, activate, off) so the frontend and agents can
//                drive the real engine now. They will be replaced by the
//                shared route handlers from the API phase (Phase 2.3); the
//                response shapes follow ARCHITECTURE field names but are not
//                the OpenAPI contract yet.
//   /sim/...     simulator controls (remote buttons, time, logs, scripts,
//                reset). Never exist on the hub.

#include <functional>
#include <memory>
#include <vector>

#include "host_guard.h"
#include "hubbo/http/route.h"
#include "scenario.h"

namespace hubbo::sim {

class SimApi {
public:
    SimApi(ScenarioRunner &runner, const char *authMode);

    void handle(http::Request &request, http::Response &response) const { router_.dispatch(request, response); }

private:
    class Fn : public http::RouteHandler {
    public:
        using F = std::function<void(const http::Request &, const http::PathParams &, http::Response &)>;
        explicit Fn(F f) : f_(std::move(f)) {}
        void handle(const http::Request &r, const http::PathParams &p, http::Response &out) override { f_(r, p, out); }

    private:
        F f_;
    };

    void api(const char *pattern, auth::Operation operation, Fn::F f);
    void host(http::Method method, const char *pattern, Fn::F f);
    std::string simTime() const;

    ScenarioRunner &runner_;
    const char *authMode_;
    http::Router router_;
    std::vector<std::unique_ptr<Fn>> handlers_;
};

}  // namespace hubbo::sim
