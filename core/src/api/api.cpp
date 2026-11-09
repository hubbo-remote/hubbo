#include "hubbo/api/api.h"

#include "routes.h"
#include "support.h"

namespace hubbo::api {

Api::Api(ApiContext context) : ctx_(context) {}
Api::~Api() = default;

void Api::registerRoutes(http::Router &router) {
    RouteSink sink{router, handlers_, ctx_.config, ctx_.clock};
    addAuthRoutes(sink, ctx_);
    addRegistryRoutes(sink, ctx_);
    addActivityRoutes(sink, ctx_);
    addStatusRoutes(sink, ctx_);
    addBackupRoutes(sink, ctx_);
    addUnimplementedRoutes(sink);
}

// ---------------------------------------------------------------- 501s

// The operations api/openapi.yaml marks `x-hubbo-not-implemented: true`
// (api/lint.sh checks the two lists match).
void addUnimplementedRoutes(RouteSink &sink) {
    struct Pending {
        const char *pattern;
        auth::Operation op;
        const char *message;
    };
    static constexpr Pending kPending[] = {
        {"/api/v1/admin/integrations/{integration_id}", auth::Operation::GetIntegrationConfig,
         "Integration settings are not implemented yet"},
        {"/api/v1/admin/integrations/{integration_id}", auth::Operation::ConfigureIntegration,
         "Integration settings are not implemented yet"},
        {"/api/v1/admin/integrations/{integration_id}/discover", auth::Operation::DiscoverIntegration,
         "Integration discovery is not implemented yet"},
        {"/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::StartIntegrationPairing,
         "Bluetooth pairing is not implemented yet"},
        {"/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::ForgetIntegrationPairing,
         "Bluetooth pairing is not implemented yet"},
        {"/api/v1/admin/remote/pair", auth::Operation::GetRemotePairing, "Remote pairing is not implemented yet"},
        {"/api/v1/admin/remote/pair", auth::Operation::StartRemotePairing, "Remote pairing is not implemented yet"},
        {"/api/v1/admin/remote/pair", auth::Operation::CancelRemotePairing, "Remote pairing is not implemented yet"},
        {"/api/v1/admin/firmware", auth::Operation::GetFirmwareStatus, "Firmware status is not implemented yet"},
        {"/api/v1/admin/firmware/update", auth::Operation::UpdateFirmware, "Firmware update is not implemented yet"},
    };
    for (const auto &p : kPending) {
        const char *message = p.message;
        sink.add(p.pattern, p.op, [message](auto &, auto &, auto &out) {
            sendError(out, 501, "not_implemented", message);
        });
    }
}

}  // namespace hubbo::api
