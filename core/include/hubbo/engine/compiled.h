#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "hubbo/domain/config.h"
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

// The one stored copy of `call` (config or HTTP task; never the engine).
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
    // A Home Assistant action's call (call_service), shared with every copy
    // of this action (executor nodes, the adapter's hand-over), so work that
    // outlives a config publish still has it. Empty for every other command.
    HaServiceCallRef call;
    // A smart +/- tap's int `step` param (light.brightness_step,
    // cover.position_step), which merged taps scale; -1 everywhere else.
    int8_t stepParam = -1;
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
    // Buttons with any trigger bound to at least one action (an empty list
    // is unbound): an activity's mask those buttons' global bindings entirely.
    remote::ButtonSet bound = 0;

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

// A smart-button press (SmartPressActions): Tap, Hold and the Up after a Hold.
struct CompiledSmartPress {
    ActionSpan tap;
    ActionSpan hold;
    ActionSpan release;
    bool merges = false;  // + or -, and a tap action has a step: quick taps merge

    // Has any actions: then this press owns its button (no global binding).
    bool set() const { return tap.count != 0 || hold.count != 0 || release.count != 0; }
};

// A slot's presses: its own button, and + and - while it's selected.
enum class SmartPress : uint8_t { Button, Plus, Minus, Count };

struct CompiledSmartSlot {
    CompiledSmartPress presses[static_cast<size_t>(SmartPress::Count)];

    CompiledSmartPress &at(SmartPress p) { return presses[static_cast<size_t>(p)]; }
    const CompiledSmartPress &at(SmartPress p) const { return presses[static_cast<size_t>(p)]; }
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
    CompiledSmartSlot smartSlots[kSmartSlotCount];  // in SmartSlot order
    std::vector<CompiledAction> actions;      // every span points in here

    int findActivity(std::string_view id) const;  // index or -1 (cold path)

    const CompiledAction *span(const ActionSpan &s) const { return actions.data() + s.offset; }

    // The binding for a trigger. Fallback is per button: if the active
    // activity binds any trigger of the button, only its bindings apply (an
    // unbound kind gives an empty span); else the global binding. So a
    // pass-through :down/:up never runs alongside a global :tap or :hold.
    // `activity` may be -1 (no activity active). `fromGlobal` (optional)
    // reports which table answered.
    ActionSpan lookup(int activity, remote::Button button, remote::EventKind kind,
                      bool *fromGlobal = nullptr) const;

    // Whether the activity binds any trigger of the button (it then owns
    // the button: no smart button or global binding applies). -1: none.
    bool activityBinds(int activity, remote::Button button) const;

    // The smart-button press that owns `button` when the activity doesn't:
    // a slot button's own, or `selected`'s +/-; nullptr when it has no
    // actions (the global bindings then apply) or isn't a smart button.
    // `slot` and `press` (optional) say which it is.
    const CompiledSmartPress *smartPress(remote::Button button, SmartSlot selected, SmartSlot *slot = nullptr,
                                         SmartPress *press = nullptr) const;
};

}  // namespace hubbo
