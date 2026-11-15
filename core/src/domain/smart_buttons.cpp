#include "hubbo/domain/smart_buttons.h"

namespace hubbo {

namespace {

// A slot is named after its button ("light_1", ...).
constexpr remote::Button kButtons[kSmartSlotCount] = {remote::Button::Light1, remote::Button::Light2,
                                                      remote::Button::Switch1, remote::Button::Switch2};

}  // namespace

const char *smartSlotName(SmartSlot slot) {
    const auto i = static_cast<size_t>(slot);
    return remote::buttonName(i < kSmartSlotCount ? kButtons[i] : kButtons[0]);
}

bool smartSlotForButton(remote::Button button, SmartSlot *out) {
    for (size_t i = 0; i < kSmartSlotCount; i++) {
        if (button == kButtons[i]) {
            *out = static_cast<SmartSlot>(i);
            return true;
        }
    }
    return false;
}

}  // namespace hubbo
