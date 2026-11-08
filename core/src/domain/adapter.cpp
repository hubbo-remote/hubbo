#include "hubbo/domain/adapter.h"

#include "hubbo/domain/ids.h"

namespace hubbo {

const char *execStatusName(ExecStatus status) {
    switch (status) {
        case ExecStatus::Accepted: return "accepted";
        case ExecStatus::Pending: return "pending";
        case ExecStatus::Failed: return "failed";
        case ExecStatus::Timeout: return "timeout";
    }
    return "failed";
}

ErrorCode AdapterSet::add(IntegrationAdapter &adapter) {
    const char *id = adapter.integrationId();
    if (id == nullptr || !isValidId(id) || std::string_view(id) == kSystemIntegrationId) {
        return ErrorCode::InvalidId;
    }
    if (find(id) >= 0) {
        return ErrorCode::DuplicateId;
    }
    if (count_ == limits::kMaxAdapters) {
        return ErrorCode::CapacityExceeded;
    }
    adapters_[count_++] = &adapter;
    return ErrorCode::Ok;
}

int AdapterSet::find(std::string_view integrationId) const {
    for (size_t i = 0; i < count_; i++) {
        if (integrationId == adapters_[i]->integrationId()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void AdapterSet::releaseAll(uint32_t nowMs) {
    for (size_t i = 0; i < count_; i++) {
        adapters_[i]->releaseAll(nowMs);
    }
}

void AdapterSet::poll(uint32_t nowMs, CompletionSink &sink) {
    for (size_t i = 0; i < count_; i++) {
        adapters_[i]->poll(nowMs, sink);
    }
}

}  // namespace hubbo
