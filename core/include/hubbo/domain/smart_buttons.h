#pragma once

#include <cstddef>
#include <cstdint>

#include "hubbo/remote/button.h"

// Smart buttons (docs/design/smart-buttons.md): the remote's Light 1/2 and
// Switch 1/2 are slots, each with its own button press and what +/- do
// while it's the selected slot. The slots' config is SmartButtonsSpec
// (domain/config.h).
namespace hubbo {

enum class SmartSlot : uint8_t { Light1, Light2, Switch1, Switch2, Count };
constexpr size_t kSmartSlotCount = static_cast<size_t>(SmartSlot::Count);

const char *smartSlotName(SmartSlot slot);  // "light_1", "light_2", "switch_1", "switch_2"
// The slot a remote button is (Light1 -> light_1, ...); false for any other.
bool smartSlotForButton(remote::Button button, SmartSlot *out);

}  // namespace hubbo
