#pragma once

// Helpers shared by the API handlers (not a public header).

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

#include "hubbo/api/api.h"
#include "hubbo/domain/error.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/http/route.h"

namespace hubbo::config {
struct Outcome;
}

namespace hubbo::api {

// ---- responses ------------------------------------------------------------

void sendJson(http::Response &out, int status, const JsonDocument &doc);
void sendEmpty(http::Response &out, int status);  // 204 and friends
void sendError(http::Response &out, int status, std::string_view code, std::string_view message);
// The Error envelope's inner object ({"error":{code,message}}), for callers
// that add fields (details, current_revision, retry_after_s).
JsonObject errorBody(JsonDocument &doc, std::string_view code, std::string_view message);

// [{path, code, message}] for each issue (ErrorDetail in the spec).
void putIssues(JsonArray out, const ValidationReport &report);
// 422 validation_failed with details from `report`.
void sendIssues(http::Response &out, const ValidationReport &report, std::string_view message);
// 422 validation_failed with one detail whose code isn't a core ErrorCode
// (e.g. weak_password, pattern).
void sendDetail(http::Response &out, std::string_view path, std::string_view code, std::string_view message);

// A query value of 1-10 decimal digits (no sign, no spaces) within `max`.
bool parseDecimal(std::string_view text, uint64_t max, uint64_t *value);

// The one ErrorCode -> HTTP mapping for config/engine results: 400, 404,
// 409 revision_conflict / device_in_use / activity_in_use / limit_reached /
// busy, 422 with details, 500, 501, 503. `what` names the object ("activity").
void sendCoreError(http::Response &out, ErrorCode code, std::string_view what);
void sendOutcome(http::Response &out, const config::Outcome &outcome, std::string_view what);

// Human text for a validation detail code.
const char *issueMessage(ErrorCode code);

// RFC 3339 UTC ("2026-11-08T20:15:00Z"), or "" if unixSeconds < 0.
std::string formatTimestamp(int64_t unixSeconds);
// obj[key] = formatTimestamp(t), or null. (Takes the object and key: a
// JsonVariant from doc["key"] is unbound until assigned through the proxy.)
void putTimestamp(JsonObject obj, const char *key, int64_t unixSeconds);
// obj[key] = id, or null if id is empty.
void putNullableId(JsonObject obj, const char *key, std::string_view id);

// Appends {id, type, state} for an integration (nullptr = the built-in
// system integration) and returns it for callers that add more fields.
JsonObject putIntegrationSummary(JsonArray list, const ApiContext &ctx, const IntegrationSpec *integration);

// ConfigState: {state, generation, reason?} from the ConfigService.
void putConfigState(JsonObject out, const ApiContext &ctx);

// "enforced" or "simulator_no_auth" (api/openapi.yaml AuthMode).
const char *authMode(const ApiContext &ctx);

// ---- requests -------------------------------------------------------------

// Parses the body (size and Content-Type were checked by the router). On
// failure 400 malformed_json is sent and false returned. An empty body with
// `optional` set yields an empty object.
bool readJson(const http::Request &request, http::Response &out, JsonDocument &doc, bool optional = false);

// Strict checks for small fixed-shape objects (auth bodies, execute).
struct Field {
    enum class Kind : uint8_t { String, Bool, Integer, Object };
    const char *name;
    Kind kind;
    bool required;
    size_t maxLength = 0;  // String: bytes (0 = no bound)
};

bool checkObject(JsonVariantConst value, std::initializer_list<Field> fields, ValidationReport *report,
                 const std::string &path = "");

// Revision from a query parameter (DELETE). False (and 422 sent) if missing
// or not a positive integer.
bool queryRevision(const http::Request &request, http::Response &out, uint32_t *revision);

// The device/activity with this id, or nullptr after sending 404.
const DeviceSpec *requireDevice(const ApiContext &ctx, std::string_view id, http::Response &out);
const ActivitySpec *requireActivity(const ApiContext &ctx, std::string_view id, http::Response &out);

// Runtime::run; false after sending 503 busy if the engine queue is full.
bool runEngine(ApiContext &ctx, EngineRequest &request, http::Response &out);

}  // namespace hubbo::api
