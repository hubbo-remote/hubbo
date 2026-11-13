#include "hubbo/api/restart_port.h"

namespace hubbo::api {

bool SimulatedRestart::requestRestart() {
    if (!ready) return false;
    if (!pending_) elapsedMs_ = 0;
    pending_ = true;
    return true;
}

void SimulatedRestart::reset() {
    ready = true;
    pending_ = false;
    elapsedMs_ = 0;
}

void SimulatedRestart::advance(uint32_t elapsedMs) {
    if (!pending_) return;
    elapsedMs_ += elapsedMs;
    if (elapsedMs_ < kRestartMs) return;
    pending_ = false;
    restarts_++;
    if (restarted) restarted();
}

}  // namespace hubbo::api
