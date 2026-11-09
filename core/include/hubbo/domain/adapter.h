#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hubbo/domain/config.h"
#include "hubbo/domain/error.h"
#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"

// The integration adapter boundary: the only way an action leaves the core.
//
// An adapter is one integration instance (e.g. "ble", "ha_main"). It
// describes which commands each registered device offers (a CommandCatalog
// of typed parameters) and executes already-validated, compiled commands.
// It never receives strings to interpret: the command is a catalog entry and
// the parameters are typed values in catalog order.
//
// Threading: every method is called on the engine task. execute() must not
// block (no network waits, no sleeps); queue internally and report later.
namespace hubbo {

enum class ExecStatus : uint8_t {
    Accepted,  // handed off (e.g. HID report sent, request queued). Not a claim
               // the physical device responded.
    Pending,   // result will arrive later through CompletionSink::complete()
    Failed,
    Timeout,
};

const char *execStatusName(ExecStatus status);  // "accepted", "pending", "failed", "timeout"

struct ExecResult {
    ExecStatus status = ExecStatus::Accepted;
    ErrorCode error = ErrorCode::Ok;

    static constexpr ExecResult accepted() { return {ExecStatus::Accepted, ErrorCode::Ok}; }
    static constexpr ExecResult pending() { return {ExecStatus::Pending, ErrorCode::Ok}; }
    static constexpr ExecResult failed(ErrorCode code) { return {ExecStatus::Failed, code}; }
    static constexpr ExecResult timeout() { return {ExecStatus::Timeout, ErrorCode::AdapterTimeout}; }
};

// Identifies one pending execution. Never 0.
using Ticket = uint32_t;

struct ExecRequest {
    const CommandSpec *command;  // entry in the catalog the device was compiled against
    uint16_t commandIndex;       // its index in that catalog
    const ParamValue *params;    // command->paramCount values, in spec order
    uint8_t paramsPresent;       // bit i: params[i] was supplied, not defaulted
    const char *deviceId;        // hub device id (diagnostics)
    const char *providerRef;     // the device's internal target
    Ticket ticket;               // for Pending results
    uint32_t nowMs;
};

class CompletionSink {
public:
    virtual ~CompletionSink() = default;
    // Report the result of a Pending execution. Unknown or stale tickets
    // (cancelled by an activity switch, already timed out) are ignored.
    virtual void complete(Ticket ticket, ExecResult result, uint32_t nowMs) = 0;
};

class IntegrationAdapter {
public:
    virtual ~IntegrationAdapter() = default;

    // Stable integration_id; must satisfy isValidId and not be "system".
    virtual const char *integrationId() const = 0;

    // The integration type this adapter implements (kBleHidType,
    // kHomeAssistantType, ...). The resolver checks it against the configured
    // integration. nullptr: unknown (not checked).
    virtual const char *integrationType() const { return nullptr; }

    // Commands offered for a registered device of this integration. An empty
    // catalog means the device can't be targeted (e.g. unsupported entity).
    // The returned storage must outlive every compiled config that uses it.
    virtual CommandCatalog commandsFor(const DeviceSpec &device) const = 0;

    // Execute one compiled command. Must not block.
    virtual ExecResult execute(const ExecRequest &request) = 0;

    // Held-key safety: release anything this adapter holds down (HID keys).
    // Called on every activity transition and on lost releases. Default: the
    // adapter holds nothing.
    virtual void releaseAll(uint32_t nowMs) { (void)nowMs; }

    // Called every engine tick. Deliver Pending results here (on the engine
    // task) via sink.complete(), never from inside execute().
    virtual void poll(uint32_t nowMs, CompletionSink &sink) {
        (void)nowMs;
        (void)sink;
    }
};

// The fixed set of integration adapters, registered once at boot. Compiled
// configs refer to adapters by index into this set, so it must not change
// while any compiled config exists.
class AdapterSet {
public:
    // Rejects invalid/reserved/duplicate ids (InvalidId, DuplicateId) and a
    // full set (CapacityExceeded).
    ErrorCode add(IntegrationAdapter &adapter);

    int find(std::string_view integrationId) const;  // index or -1
    IntegrationAdapter *at(size_t index) const { return index < count_ ? adapters_[index] : nullptr; }
    size_t size() const { return count_; }

    void releaseAll(uint32_t nowMs);
    void poll(uint32_t nowMs, CompletionSink &sink);

private:
    IntegrationAdapter *adapters_[limits::kMaxAdapters] = {};
    size_t count_ = 0;
};

}  // namespace hubbo
