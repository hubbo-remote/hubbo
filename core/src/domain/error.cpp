#include "hubbo/domain/error.h"

#include <cstddef>
#include <cstring>

namespace hubbo {

namespace {

constexpr const char *kNames[] = {
    "ok",
    "invalid_id",
    "invalid_name",
    "invalid_command_id",
    "duplicate_id",
    "unknown_integration",
    "unknown_device",
    "device_required",
    "device_not_allowed",
    "integration_mismatch",
    "unknown_command",
    "unknown_param",
    "duplicate_param",
    "missing_param",
    "invalid_param_type",
    "param_out_of_range",
    "invalid_enum_value",
    "too_many_params",
    "invalid_trigger",
    "duplicate_trigger",
    "unknown_activity",
    "misplaced_transition",
    "too_many_actions",
    "too_many_activities",
    "too_many_devices",
    "capacity_exceeded",
    "no_active_activity",
    "queue_full",
    "cancelled",
    "adapter_failed",
    "adapter_timeout",
    "adapter_unavailable",
    "not_implemented",
    "busy",
    "internal",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<size_t>(ErrorCode::Count),
              "kNames must name every ErrorCode");

}  // namespace

const char *errorCodeName(ErrorCode code) {
    const auto index = static_cast<size_t>(code);
    return index < static_cast<size_t>(ErrorCode::Count) ? kNames[index] : "internal";
}

bool errorCodeFromName(const char *name, ErrorCode *out) {
    if (name == nullptr) return false;
    for (size_t i = 0; i < static_cast<size_t>(ErrorCode::Count); i++) {
        if (std::strcmp(kNames[i], name) == 0) {
            *out = static_cast<ErrorCode>(i);
            return true;
        }
    }
    return false;
}

}  // namespace hubbo
