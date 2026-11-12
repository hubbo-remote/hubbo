// The built-in device profiles and the device kinds (config/profile.h).

#include "hubbo/config/profile.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

void addProfileRoutes(RouteSink &sink, ApiContext &) {
    sink.add("/api/v1/profiles", auth::Operation::ListProfiles, [](auto &, auto &, auto &out) {
        JsonDocument doc;
        JsonArray list = doc["profiles"].to<JsonArray>();
        for (const config::BuiltinProfile &p : config::builtinProfiles().profiles()) {
            const config::ProfileDocument &d = p.document;
            JsonObject o = list.add<JsonObject>();
            o["id"] = d.profile.id;
            o["revision"] = d.profile.revision;
            o["sha256"] = d.profile.sha256;
            o["manufacturer"] = d.profile.manufacturer;
            o["model"] = d.profile.model;
            o["kind"] = d.kind;
            o["platform"] = d.platform;
        }
        sendJson(out, 200, doc);
    });

    // The published file without `meta` (the hub keeps none).
    sink.add("/api/v1/profiles/{profile_id}", auth::Operation::GetProfile, [](auto &, auto &p, auto &out) {
        const std::string id = http::percentDecode(p[0]);  // clients may send ':' as %3A
        const config::BuiltinProfile *profile = config::builtinProfiles().find(id);
        if (profile == nullptr) return sendError(out, 404, "not_found", "There's no profile " + id);
        JsonDocument doc;
        if (deserializeJson(doc, profile->json) != DeserializationError::Ok) {
            return sendError(out, 500, "internal_error", "The built-in profile can't be read");
        }
        doc.remove("meta");
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/kinds", auth::Operation::ListKinds, [](auto &, auto &, auto &out) {
        JsonDocument doc;
        JsonArray list = doc["kinds"].to<JsonArray>();
        for (const config::DeviceKind &k : config::builtinProfiles().kinds()) {
            JsonObject o = list.add<JsonObject>();
            o["id"] = k.id;
            o["label"] = k.label;
        }
        sendJson(out, 200, doc);
    });
}

}  // namespace hubbo::api
