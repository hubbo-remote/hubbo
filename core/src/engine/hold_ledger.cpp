#include "hubbo/engine/hold_ledger.h"

#include <string_view>

namespace hubbo {

namespace {

bool sameParams(const CommandSpec *command, const ParamValue *a, const ParamValue *b) {
    for (uint8_t i = 0; i < command->paramCount; i++) {
        if (a[i].type != b[i].type || a[i].integer != b[i].integer || a[i].number != b[i].number) return false;
    }
    return true;
}

}  // namespace

bool HoldLedger::add(const OpenHold &hold) {
    for (size_t i = 0; i < kCapacity; i++) {
        if (!used_[i]) {
            holds_[i] = hold;
            used_[i] = true;
            return true;
        }
    }
    return false;
}

int HoldLedger::match(uint8_t adapterIndex, const char *deviceId, const CommandSpec *command,
                      const ParamValue *params) const {
    if (command == nullptr) return -1;
    for (size_t i = 0; i < kCapacity; i++) {
        const OpenHold &h = holds_[i];
        if (!used_[i] || h.releasingSequence != 0 || h.adapterIndex != adapterIndex || h.release == nullptr ||
            std::string_view(h.release->id) != command->id || h.deviceId != deviceId ||
            !sameParams(command, h.params, params)) {
            continue;
        }
        return static_cast<int>(i);
    }
    return -1;
}

void HoldLedger::erase(int slot) {
    if (slot >= 0 && static_cast<size_t>(slot) < kCapacity) used_[slot] = false;
}

void HoldLedger::markReleasing(int slot, uint32_t sequenceId, uint8_t index) {
    if (slot < 0 || static_cast<size_t>(slot) >= kCapacity || !used_[slot]) return;
    holds_[slot].releasingSequence = sequenceId;
    holds_[slot].releasingIndex = index;
}

bool HoldLedger::takeReleasing(uint32_t sequenceId, OpenHold *out) {
    if (sequenceId == 0) return false;
    for (size_t i = 0; i < kCapacity; i++) {
        if (used_[i] && holds_[i].releasingSequence == sequenceId) {
            *out = holds_[i];
            used_[i] = false;
            return true;
        }
    }
    return false;
}

bool HoldLedger::take(remote::Button button, OpenHold *out) {
    for (size_t i = 0; i < kCapacity; i++) {
        if (used_[i] && holds_[i].button == button && holds_[i].releasingSequence == 0) {
            *out = holds_[i];
            used_[i] = false;
            return true;
        }
    }
    return false;
}

bool HoldLedger::takeAny(OpenHold *out) {
    for (size_t i = 0; i < kCapacity; i++) {
        if (used_[i]) {
            *out = holds_[i];
            used_[i] = false;
            return true;
        }
    }
    return false;
}

size_t HoldLedger::size() const {
    size_t n = 0;
    for (bool u : used_) n += u ? 1 : 0;
    return n;
}

}  // namespace hubbo
