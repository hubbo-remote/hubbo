#include "sim_api.h"

#include <sstream>
#include <string>

namespace hubbo::sim {

namespace {

using http::jsonString;
using http::Method;

std::string logJson(const std::vector<LogLine> &log, size_t limit) {
    std::string out = "[";
    const size_t start = log.size() > limit ? log.size() - limit : 0;
    for (size_t i = start; i < log.size(); i++) {
        if (i != start) out += ",";
        out += "{\"at_ms\":" + std::to_string(log[i].atMs) + ",\"text\":" + jsonString(log[i].text) + "}";
    }
    return out + "]";
}

api::HubInfo hubInfo(bool authEnabled) {
    api::HubInfo info;
    info.version = "0.0.0-sim";
    info.build = "hub-sim";
    info.board = "simulator";
    info.hostname = "hub-sim";
    info.simulatorNoAuth = !authEnabled;
    return info;
}

}  // namespace

SimApi::SimApi(ScenarioRunner &runner, bool authEnabled, auth::Credentials &credentials, SwitchableStorage *storage)
    : runner_(runner),
      authEnabled_(authEnabled),
      storage_(storage),
      credentials_(credentials),
      sessions_(random_),
      clock_(runner.hub()),
      runtime_(runner.hub()),
      api_(api::ApiContext{credentials_, sessions_, loginLimiter_, clock_, runner.hub().configService(), runtime_,
                           hubInfo(authEnabled)}),
      productionAuth_(credentials_, sessions_, clock_),
      noAuth_(credentials_) {
    router_.setAllowedHosts({"localhost", "127.0.0.1", "[::1]"});
    api::registerApiRoutes(router_, api_);

    host(Method::Post, "/sim/remote/{button}/{action}", [this](auto &, auto &p, auto &out) {
        const std::string action(p[1]);
        if (action != "press" && action != "release" && action != "tap") {
            http::jsonError(out, 400, "bad_request", "action must be press, release or tap");
            return;
        }
        if (!runner_.runLine(action + " " + std::string(p[0]))) {
            http::jsonError(out, 400, "bad_request", runner_.error());
            return;
        }
        out.json(200, simTime());
    });

    host(Method::Post, "/sim/advance", [this](auto &r, auto &, auto &out) {
        if (!runner_.runLine("advance " + std::string(http::queryParam(r.query, "ms")))) {
            http::jsonError(out, 400, "bad_request", runner_.error());
            return;
        }
        out.json(200, simTime());
    });

    host(Method::Get, "/sim/log", [this](auto &, auto &, auto &out) {
        SimHub &hub = runner_.hub();
        out.json(200, "{\"commands\":" + logJson(hub.commands(), 200) + ",\"events\":" + logJson(hub.events(), 200) +
                          "}");
    });

    // Back to the seed config, nothing active, faults cleared (persisted).
    host(Method::Post, "/sim/reset", [this](auto &, auto &, auto &out) {
        runner_.runLine("reset");
        out.json(200, simTime());
    });

    // Storage health: ?health=available|unavailable|read_only. Edits then
    // fail (storage_unavailable) while the remote keeps working.
    host(Method::Post, "/sim/storage", [this](auto &r, auto &, auto &out) {
        const std::string_view wanted = http::queryParam(r.query, "health");
        storage::Health health;
        if (wanted == "available") {
            health = storage::Health::Available;
        } else if (wanted == "unavailable") {
            health = storage::Health::Unavailable;
        } else if (wanted == "read_only") {
            health = storage::Health::ReadOnly;
        } else {
            http::jsonError(out, 400, "bad_request", "health must be available, unavailable or read_only");
            return;
        }
        if (storage_ == nullptr) {
            http::jsonError(out, 409, "not_available", "this hub-sim has no switchable storage");
            return;
        }
        storage_->setHealth(health);
        out.json(200, "{\"health\":" + jsonString(storage::healthName(storage_->health())) + "}");
    });

    // Body: a scenario script, run against the live hub.
    host(Method::Post, "/sim/script", [this](auto &r, auto &, auto &out) {
        std::istringstream script{std::string(r.body)};
        if (!runner_.run(script, "request")) {
            out.json(422, "{\"ok\":false,\"error\":" + jsonString(runner_.error()) + "}");
            return;
        }
        out.json(200, "{\"ok\":true,\"sim_time_ms\":" + std::to_string(runner_.hub().nowMs()) + "}");
    });
}

http::RequestAuthenticator &SimApi::authenticator() {
    if (authEnabled_) return productionAuth_;
    return noAuth_;
}

void SimApi::host(Method method, const char *pattern, Fn::F f) {
    handlers_.push_back(std::make_unique<Fn>(std::move(f)));
    router_.addHostRoute(method, pattern, *handlers_.back());
}

std::string SimApi::simTime() const {
    return "{\"sim_time_ms\":" + std::to_string(runner_.hub().nowMs()) + "}";
}

}  // namespace hubbo::sim
