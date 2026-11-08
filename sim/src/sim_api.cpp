#include "sim_api.h"

#include <sstream>
#include <string>

namespace hubbo::sim {

namespace {

using http::jsonString;
using http::Method;
using Operation = auth::Operation;

std::string logJson(const std::vector<LogLine> &log, size_t limit) {
    std::string out = "[";
    const size_t start = log.size() > limit ? log.size() - limit : 0;
    for (size_t i = start; i < log.size(); i++) {
        if (i != start) out += ",";
        out += "{\"at_ms\":" + std::to_string(log[i].atMs) + ",\"text\":" + jsonString(log[i].text) + "}";
    }
    return out + "]";
}

}  // namespace

void SimApi::api(const char *pattern, Operation operation, Fn::F f) {
    handlers_.push_back(std::make_unique<Fn>(std::move(f)));
    router_.add(pattern, operation, *handlers_.back());
}

void SimApi::host(Method method, const char *pattern, Fn::F f) {
    handlers_.push_back(std::make_unique<Fn>(std::move(f)));
    router_.addHostRoute(method, pattern, *handlers_.back());
}

std::string SimApi::simTime() const {
    return "{\"sim_time_ms\":" + std::to_string(runner_.hub().nowMs()) + "}";
}

SimApi::SimApi(ScenarioRunner &runner, const char *authMode) : runner_(runner), authMode_(authMode) {
    // ---- provisional /api/v1 ----
    api("/api/v1/status", Operation::GetStatus, [this](auto &, auto &, auto &out) {
        SimHub &hub = runner_.hub();
        Engine &e = hub.engine();
        const ExecutorStats &s = e.executor().stats();
        std::string body = "{";
        body += "\"auth_mode\":" + jsonString(authMode_);
        body += ",\"simulator\":true";
        body += ",\"active_activity_id\":" + (e.hasActiveActivity() ? jsonString(e.activeActivityId()) : "null");
        body += ",\"config_revision\":" + std::to_string(hub.config().revision);
        body += ",\"sim_time_ms\":" + std::to_string(hub.nowMs());
        body += ",\"executor\":{\"enqueued\":" + std::to_string(s.enqueued) +
                ",\"completed\":" + std::to_string(s.completed) + ",\"failed\":" + std::to_string(s.failed) +
                ",\"cancelled\":" + std::to_string(s.cancelled) + ",\"rejected\":" + std::to_string(s.rejected) +
                ",\"timeouts\":" + std::to_string(s.timeouts) + "}";
        body += "}";
        out.json(200, body);
    });

    api("/api/v1/activities", Operation::ListActivities, [this](auto &, auto &, auto &out) {
        std::string body = "[";
        const auto &activities = runner_.hub().config().activities;
        for (size_t i = 0; i < activities.size(); i++) {
            if (i != 0) body += ",";
            body += "{\"id\":" + jsonString(activities[i].id) + ",\"name\":" + jsonString(activities[i].name) +
                    ",\"revision\":" + std::to_string(activities[i].revision) + "}";
        }
        out.json(200, body + "]");
    });

    api("/api/v1/activities/{id}/activate", Operation::ActivateActivity, [this](auto &, auto &p, auto &out) {
        const ErrorCode code = runner_.hub().activate(std::string(p[0]));
        if (code == ErrorCode::UnknownActivity) {
            http::jsonError(out, 404, errorCodeName(code), "no such activity");
        } else if (code != ErrorCode::Ok) {
            http::jsonError(out, 503, errorCodeName(code), "activity selected but startup could not be queued");
        } else {
            out.json(202, "{\"active_activity_id\":" + jsonString(p[0]) + "}");
        }
    });

    api("/api/v1/activities/off", Operation::ActivitiesOff, [this](auto &, auto &, auto &out) {
        const ErrorCode code = runner_.hub().off();
        if (code == ErrorCode::NoActiveActivity) {
            http::jsonError(out, 409, errorCodeName(code), "no activity is active");
        } else {
            out.json(202, "{\"active_activity_id\":null}");
        }
    });

    // ---- simulator controls ----
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

    host(Method::Post, "/sim/reset", [this](auto &, auto &, auto &out) {
        runner_.runLine("reset");
        out.json(200, simTime());
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

}  // namespace hubbo::sim
