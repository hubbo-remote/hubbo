#include "hubbo/engine/compiled.h"

namespace hubbo {

int CompiledConfig::findActivity(std::string_view id) const {
    for (size_t i = 0; i < activities.size(); i++) {
        if (activities[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool CompiledConfig::activityBinds(int activity, remote::Button button) const {
    return activity >= 0 && static_cast<size_t>(activity) < activities.size() &&
           (activities[static_cast<size_t>(activity)].table.bound & remote::buttonBit(button)) != 0;
}

const CompiledSmartPress *CompiledConfig::smartPress(remote::Button button, SmartSlot selected, SmartSlot *slotOut,
                                                     SmartPress *pressOut) const {
    SmartSlot slot = selected;
    SmartPress press = SmartPress::Button;
    if (button == remote::Button::Plus) {
        press = SmartPress::Plus;
    } else if (button == remote::Button::Minus) {
        press = SmartPress::Minus;
    } else if (!smartSlotForButton(button, &slot)) {
        return nullptr;
    }
    const CompiledSmartPress &found = smartSlots[static_cast<size_t>(slot)].at(press);
    if (!found.set()) return nullptr;
    if (slotOut != nullptr) *slotOut = slot;
    if (pressOut != nullptr) *pressOut = press;
    return &found;
}

ActionSpan CompiledConfig::lookup(int activity, remote::Button button, remote::EventKind kind,
                                  bool *fromGlobal) const {
    if (static_cast<size_t>(button) >= remote::kButtonCount ||
        static_cast<size_t>(kind) >= remote::kEventKindCount) {
        return {};
    }
    if (activity >= 0 && static_cast<size_t>(activity) < activities.size()) {
        const TriggerTable &table = activities[static_cast<size_t>(activity)].table;
        if ((table.bound & remote::buttonBit(button)) != 0) {
            if (fromGlobal != nullptr) *fromGlobal = false;
            return table.at(button, kind);
        }
    }
    if (fromGlobal != nullptr) *fromGlobal = true;
    return globals.at(button, kind);
}

}  // namespace hubbo
