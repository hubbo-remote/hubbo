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
    addProfileRoutes(sink, ctx_);
    addActivityRoutes(sink, ctx_);
    addSmartButtonRoutes(sink, ctx_);
    addStatusRoutes(sink, ctx_);
    addBackupRoutes(sink, ctx_);
    addIntegrationRoutes(sink, ctx_);
    addRemoteRoutes(sink, ctx_);
    addBleRoutes(sink, ctx_);
    addFirmwareRoutes(sink, ctx_);
    addHubRoutes(sink, ctx_);
    addDocsRoutes(sink, ctx_);
}

}  // namespace hubbo::api
