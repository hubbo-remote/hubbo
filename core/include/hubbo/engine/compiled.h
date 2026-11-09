#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"
#include "hubbo/domain/trigger.h"

// Runtime form of a configuration, produced by compileConfig() off the hot
// path and published through a SnapshotStore. Immutable once published.
//
// Dispatch is two array indexes: activities[active].table[button][kind]
// gives a span into `actions`. No strings, JSON, allocation or I/O.
namespace hubbo {

enum class ActionOp : uint8_t {
    Adapter,   // integration command; adapterIndex/commandIndex valid
    Wait,      // system.wait; params[0] = duration_ms
    Activate,  // system.activate; params[0] = activity index
    Off,       // system.off
    // A command of a device whose integration is disabled: reported as
    // integration_disabled, never reaches the adapter; the sequence goes on.
    Unavailable,
};

constexpr uint16_t kNoDevice = 0xFFFF;

struct CompiledAction {
    ActionOp op = ActionOp::Adapter;
    uint8_t adapterIndex = 0;       // into AdapterSet (Adapter only)
    uint16_t deviceIndex = kNoDevice;  // into CompiledConfig::devices
    uint16_t commandIndex = 0;      // into the device's catalog
    const CommandSpec *command = nullptr;  // that catalog entry (adapter-owned storage)
    ParamValue params[limits::kMaxParams];  // command->paramCount used, spec order
    uint8_t paramsPresent = 0;  // bit i: params[i] was supplied (not defaulted)
    // Hold-effect commands only: what releases the hold (same params).
    const CommandSpec *release = nullptr;
    uint16_t releaseIndex = 0;
};

// One registered device command compiled for direct execution
// (POST /devices/{id}/commands/{command_id}/execute). Self-contained: it
// carries its target, so it doesn't reference any compiled config.
struct CompiledCommand {
    CompiledAction action;
    IdString deviceId;
    FixedString<limits::kMaxProviderRefLength> providerRef;
};

struct ActionSpan {
    uint16_t offset = 0;
    uint8_t count = 0;
};

struct TriggerTable {
    ActionSpan spans[remote::kButtonCount][remote::kEventKindCount];

    const ActionSpan &at(remote::Button b, remote::EventKind k) const {
        return spans[static_cast<size_t>(b)][static_cast<size_t>(k)];
    }
};

struct CompiledDevice {
    IdString id;
    FixedString<limits::kMaxProviderRefLength> providerRef;
};

struct CompiledActivity {
    IdString id;
    FixedString<limits::kMaxNameLength> name;
    uint32_t revision = 0;
    ActionSpan startup;
    ActionSpan shutdown;
    TriggerTable table;
};

struct CompiledConfig {
    uint32_t revision = 0;
    // Set only by an activity delete (ConfigService): if this was the active
    // activity, the engine switches to the next one. Any other publish that
    // drops the active activity (restore, reset, a reload) leaves the hub
    // inactive without running startup or shutdown (design part 6).
    IdString deletedActivity;
    std::vector<CompiledDevice> devices;      // sized once at compile time
    std::vector<CompiledActivity> activities;
    TriggerTable globals;
    std::vector<CompiledAction> actions;      // every span points in here

    int findActivity(std::string_view id) const;  // index or -1 (cold path)

    const CompiledAction *span(const ActionSpan &s) const { return actions.data() + s.offset; }

    // The binding for a trigger: the active activity's if it binds it, else
    // the global binding. `activity` may be -1 (no activity active).
    // `fromGlobal` (optional) reports which table matched.
    ActionSpan lookup(int activity, remote::Button button, remote::EventKind kind,
                      bool *fromGlobal = nullptr) const;
};

}  // namespace hubbo
