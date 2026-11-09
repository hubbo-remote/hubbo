#pragma once

#include <cstddef>
#include <cstdint>

#include "hubbo/domain/ids.h"
#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"
#include "hubbo/remote/button.h"

// The engine's record of holds still open (docs/design/persistence-and-
// contracts.md, section 5): a Hold-effect command (e.g. key_down) executed
// from a button's binding, with what releases it. The guarantee that no key
// stays held lives here, not in config validation: whatever the bindings
// say, when the button goes up (after its up sequence, however that ended),
// the radio times out, or the activity changes, everything still open for
// it is released.
//
// Fixed capacity, no allocation: used on the engine task's hot path.
namespace hubbo {

struct OpenHold {
    remote::Button button = remote::Button::Up;
    uint8_t adapterIndex = 0;
    const CommandSpec *release = nullptr;  // the hold's releasedBy command
    uint16_t releaseIndex = 0;
    ParamValue params[limits::kMaxParams];
    uint8_t paramsPresent = 0;
    IdString deviceId;
    FixedString<limits::kMaxProviderRefLength> providerRef;
    // A binding's release for it is in flight (Pending): which sequence and
    // action. The hold stays open until that result is known.
    uint32_t releasingSequence = 0;  // 0: none
    uint8_t releasingIndex = 0;
};

class HoldLedger {
public:
    static constexpr size_t kCapacity = 16;

    // False when full: the caller must release this hold at once.
    bool add(const OpenHold &hold);

    // The open hold (not already being released) that this release command
    // matches: same adapter, device, release command and params. Slot or -1.
    int match(uint8_t adapterIndex, const char *deviceId, const CommandSpec *command, const ParamValue *params) const;
    // A release that completed: the hold is closed.
    void erase(int slot);
    // A release whose result is still pending (sequence `sequenceId`, action
    // `index`): kept open until takeReleasing() resolves it.
    void markReleasing(int slot, uint32_t sequenceId, uint8_t index);
    // Removes one hold whose release was in flight in `sequenceId`.
    bool takeReleasing(uint32_t sequenceId, OpenHold *out);

    // Removes one hold open for `button` (takeAny: any button) into `out`;
    // false if none. One at a time, so releasing never needs more than one
    // OpenHold on the engine task's stack. take() skips holds whose release
    // is in flight (their sequence resolves them); takeAny() doesn't.
    bool take(remote::Button button, OpenHold *out);
    bool takeAny(OpenHold *out);

    size_t size() const;

private:
    OpenHold holds_[kCapacity];
    bool used_[kCapacity] = {};
};

}  // namespace hubbo
