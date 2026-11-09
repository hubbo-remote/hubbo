#include "hubbo/api/api.h"

#include "routes.h"
#include "support.h"

namespace hubbo::api {

Api::Api(ApiContext context) : ctx_(context) { ctx_.discovery = std::make_shared<DiscoveryCache>(); }
Api::~Api() = default;

void Api::setYield(std::function<void()> yield) { ctx_.discovery->yield = std::move(yield); }

void Api::registerRoutes(http::Router &router) {
    RouteSink sink{router, handlers_, ctx_.config, ctx_.clock};
    addAuthRoutes(sink, ctx_);
    addRegistryRoutes(sink, ctx_);
    addActivityRoutes(sink, ctx_);
    addStatusRoutes(sink, ctx_);
    addBackupRoutes(sink, ctx_);
    addIntegrationRoutes(sink, ctx_);
    addRemoteRoutes(sink, ctx_);
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
        {"/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::StartIntegrationPairing,
         "Bluetooth pairing is not implemented yet"},
        {"/api/v1/admin/integrations/{integration_id}/pair", auth::Operation::ForgetIntegrationPairing,
         "Bluetooth pairing is not implemented yet"},
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
