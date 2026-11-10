#include "hubbo/api/factory_reset_port.h"

namespace hubbo::api {

bool SimulatedFactoryReset::requestFactoryReset() {
    if (!ready) return false;
    if (!pending_) elapsedMs_ = 0;
    pending_ = true;
    return true;
}

void SimulatedFactoryReset::advance(uint32_t elapsedMs) {
    if (!pending_) return;
    elapsedMs_ += elapsedMs;
    if (elapsedMs_ < kRestartMs) return;
    pending_ = false;
    if (erase) erase();
}

}  // namespace hubbo::api
